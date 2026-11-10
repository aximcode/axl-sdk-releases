/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-internal.h
    Shared AxlSsh internals — wire codec, message numbers, connection state.

    NOT a public header: it is included by src/net/axl-ssh-*.c and by the unit
    test, never by an application. The public surface is <axl/axl-ssh-core.h>
    and <axl/axl-ssh-server.h>.

    Nothing here may assume a role. These are the pieces the P6 client will
    consume unchanged, which is the whole reason the spec fixes the header
    split before there is a client to split for.
**/
#ifndef AXL_SSH_INTERNAL_H
#define AXL_SSH_INTERNAL_H

#include <axl/axl-macros.h>
#include <axl/axl-str.h>      /* axl_memcmp */
#include <axl/axl-string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * SSH wire codec (RFC 4251 section 5)
 *
 * The WRITERS trust their caller: every byte they emit is ours, and a NULL
 * builder is a programming error, not input. The READERS do not trust anything
 * — they parse bytes an unauthenticated peer chose, before any key exchange has
 * happened. So the readers validate every pointer and every length, and they
 * advance *off ONLY on success: a caller that ignores the status cannot
 * silently consume a malformed field and carry on parsing at a bogus offset.
 * ------------------------------------------------------------------------- */

/** Append a big-endian uint32. @return AXL_OK, or AXL_ERR on allocation failure. */
int axl_ssh_put_u32(AxlString *b, uint32_t v);

/** Read a big-endian uint32 at *off, advancing *off by 4 on success.
    @return AXL_OK, or AXL_ERR on a NULL argument or fewer than 4 bytes left. */
int axl_ssh_get_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *out);

/** Append an SSH string: a big-endian uint32 length followed by @a n raw bytes.
    @return AXL_OK, or AXL_ERR on allocation failure or a length exceeding the
    32-bit wire field. */
int axl_ssh_put_string(AxlString *b, const void *s, size_t n);

/** Read an SSH string, yielding a pointer INTO @a p (no copy, no allocation)
    and advancing *off past the whole field on success.
    @return AXL_OK, or AXL_ERR on a NULL argument, a truncated length prefix, or
    a length field that runs past the end of @a p. */
int axl_ssh_get_string(const uint8_t *p, size_t len, size_t *off,
                       const uint8_t **out, uint32_t *out_len);

/* ---------------------------------------------------------------------------
 * Version exchange (RFC 4253 section 4.2)
 *
 * Role-neutral on purpose: a client parses the server's identification line
 * with exactly this function, so it lives in the core rather than in the
 * server, and P6 consumes it unchanged.
 * ------------------------------------------------------------------------- */

/** Our identification line, sent before anything else. */
#define AXL_SSH_IDENT      "SSH-2.0-AxlSsh_1.0"

/** RFC 4253 section 4.2: an identification line is at most 255 bytes,
    INCLUDING the terminating CRLF. */
#define AXL_SSH_IDENT_MAX  255

/** Total bytes of pre-identification lines we are willing to scan. The RFC
    permits a peer to send arbitrary lines first (banners, legal notices) and
    does not bound how many. Unbounded, that is a peer who can keep us reading
    and buffering forever without ever identifying itself. */
#define AXL_SSH_PREAMBLE_MAX  4096

/** Find the peer's identification line, skipping any preamble lines.

    @return AXL_OK with *end_off set just past the line's CRLF; AXL_INCOMPLETE
    when the bytes so far are well-formed but no complete line has arrived yet;
    AXL_ERR when the peer is not SSH-2.0, a line exceeds #AXL_SSH_IDENT_MAX --
    whether or not it is terminated yet -- or the preamble exceeds
    #AXL_SSH_PREAMBLE_MAX. */
int axl_ssh_parse_ident(const uint8_t *p, size_t len, size_t *end_off);

/* ---------------------------------------------------------------------------
 * Binary packet protocol (RFC 4253 section 6)
 *
 * The UNENCRYPTED form, used before NEWKEYS:
 *     uint32 packet_length | byte padding_length | payload | random padding
 * The AEAD form that replaces it after NEWKEYS is further down, under
 * chacha20-poly1305@openssh.com -- it frames the same fields but encrypts the
 * length separately, so the two cannot share a wrap/unwrap.
 * ------------------------------------------------------------------------- */

/** RFC 4253 section 6.1: every implementation must handle a total packet size
    of 35000 bytes, so that is where we stop. */
#define AXL_SSH_PACKET_MAX  35000u

/** Cipher block size for the unencrypted form. The total packet length must be
    a multiple of this. */
#define AXL_SSH_BLOCK       8u

/** RFC 4253 section 6: "There MUST be at least four bytes of padding." */
#define AXL_SSH_PAD_MIN     4u

/** Frame @a payload into @a out. Padding brings the total to a multiple of
    #AXL_SSH_BLOCK and is always at least #AXL_SSH_PAD_MIN bytes.
    @return AXL_OK, or AXL_ERR on allocation failure, a payload too large to
    frame, or an RNG failure (padding must be random, so a silent zero-fill is
    not an acceptable fallback). */
int axl_ssh_packet_wrap(AxlString *out, const void *payload, size_t len);

/** Read one framed packet, yielding a pointer INTO @a p (no copy).
    @return AXL_OK with *consumed set to the whole frame; AXL_INCOMPLETE when
    the frame has not fully arrived; AXL_ERR on a NULL argument or any RFC 4253
    section 6 violation — length out of range, padding below the minimum or
    past the packet, or a total that is not a multiple of #AXL_SSH_BLOCK. */
int axl_ssh_packet_unwrap(const uint8_t *p, size_t len, size_t *consumed,
                          const uint8_t **payload, uint32_t *payload_len);

/* ---------------------------------------------------------------------------
 * Key exchange (RFC 4253 sections 7-8)
 * ------------------------------------------------------------------------- */

#define AXL_SSH_MSG_KEXINIT  20
#define AXL_SSH_MSG_NEWKEYS  21

/** A KEXINIT carries ten name-lists, in this order. Named because the
    selector must walk ALL of them to know the message is well formed, not
    only the ones it cares about. */
#define AXL_SSH_KEXINIT_LISTS  10

/** OpenSSH's strict-KEX pseudo-algorithm names (PROTOCOL section 1.10,
    "transport: strict key exchange extension"; the mitigation for Terrapin,
    CVE-2023-48795). They are not key exchange methods: each side appends its
    OWN name to the kex_algorithms list of the INITIAL KEXINIT purely to say
    "I enforce the two rules", and MUST ignore the name in any later KEXINIT.

    Never put these in the selector's `want[]` table. A peer that offers only
    a pseudo-name has offered no kex method at all, and matching one as if it
    were curve25519-sha256 would negotiate a connection with no key
    exchange. */
#define AXL_SSH_KEX_STRICT_S  "kex-strict-s-v00@openssh.com"
#define AXL_SSH_KEX_STRICT_C  "kex-strict-c-v00@openssh.com"

/** What axl_ssh_kexinit_select learned about the peer beyond "acceptable".

    One struct rather than two `bool *` out-parameters: adjacent out-params of
    the same type are a silent-swap hazard, and the caller reads both by name
    here. */
typedef struct {
    /** RFC 4253 section 7.1's first_kex_packet_follows case: the peer set the
        flag AND its first kex or host key name is not ours, so its next
        packet is a guess for a different method and MUST be dropped. Reading
        the flag is not optional -- a client that uses the optimisation cannot
        connect at all if the guess is dispatched as a real message. */
    bool guessed_wrong;
    /** The peer's kex_algorithms list carries #AXL_SSH_KEX_STRICT_C, so
        strict KEX is in force for this connection: both sequence numbers
        reset to zero at NEWKEYS. */
    bool strict_kex;
} AxlSshKexinitPeer;

/** Build our KEXINIT payload: message number, 16-byte cookie, ten name-lists,
    first_kex_packet_follows and the reserved uint32. @a initial selects the
    connection's FIRST KEXINIT, the only one that may carry the strict-KEX
    pseudo-name (OpenSSH PROTOCOL section 1.10); a rekey's offers the real
    kex method alone.
    @return AXL_OK, or AXL_ERR on allocation or RNG failure. */
int axl_ssh_kexinit_build(AxlString *out, bool initial);

/** Check the peer's KEXINIT offers our single choice in every slot we use.
    Refuses rather than negotiates: there is one name per slot by design, and
    an algorithm matrix is a downgrade surface.

    @a out, when non-NULL, receives what else the message said -- the wrong-
    guess flag and whether the peer asked for strict KEX. It is written on
    every return, including the failures, so a caller cannot act on a stale
    value from a refused message.

    @return AXL_OK when every required slot names our choice; AXL_ERR on a NULL
    argument, a malformed message, or any slot that does not. */
int axl_ssh_kexinit_select(const uint8_t *p, size_t len,
                           AxlSshKexinitPeer *out);

/** RFC 4253 section 7.2 key derivation:
      K1   = HASH(K || H || letter || session_id)
      Kn+1 = HASH(K || H || K1 || ... || Kn)
    with @a letter one of 'A'..'F'. Fills out[0..out_len), chaining as many
    hash blocks as needed and truncating the last.

    @a k MUST ALREADY BE mpint-ENCODED. The RFC hashes K as an mpint, not as
    raw bytes, and this function hashes exactly what it is given -- so passing
    a raw X25519 shared secret produces keys that are perfectly self-consistent
    and reject every real client. Encoding here instead would make the function
    unusable for the hash inputs that are not mpints.

    @return AXL_OK, or AXL_ERR on a NULL argument or digest failure. */
int axl_ssh_kdf(const uint8_t *k, size_t k_len,
                const uint8_t *h, size_t h_len,
                char letter, const uint8_t *session_id, size_t sid_len,
                uint8_t *out, size_t out_len);

/** Append an mpint (RFC 4251 section 5): a two's-complement, big-endian
    integer in an SSH string, with leading zero bytes removed and a single
    0x00 prepended when the top bit of the first byte would otherwise make it
    read as negative. Zero is a zero-length string.

    @a v is an UNSIGNED big-endian magnitude -- which is what every producer
    here has (an X25519 shared secret), and why there is no sign argument.

    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_put_mpint(AxlString *b, const uint8_t *v, size_t n);

/* ---------------------------------------------------------------------------
 * Curve25519 key exchange (RFC 5656 shape, RFC 8731 names)
 * ------------------------------------------------------------------------- */

#define AXL_SSH_MSG_DISCONNECT        1
/* RFC 4253 section 11: these three may arrive at any time and are not
   errors. IGNORE and DEBUG are ignored; UNIMPLEMENTED must NOT be answered
   with another UNIMPLEMENTED, which is an unbounded ping-pong between two
   conforming peers. Defined here rather than left as bare numbers because
   axl_ssh_unimplemented_due has to name all three to exclude them -- until
   it did, none of them existed anywhere in src/net/.

   During key exchange all three are FATAL instead, which is strict KEX rule
   (a); see on_payload. */
#define AXL_SSH_MSG_IGNORE            2
#define AXL_SSH_MSG_UNIMPLEMENTED     3
#define AXL_SSH_MSG_DEBUG             4
#define AXL_SSH_MSG_SERVICE_REQUEST   5
#define AXL_SSH_MSG_SERVICE_ACCEPT    6
/* RFC 8308. AxlSsh advertises no ext-info, so it neither sends nor expects
   one; named because a rekey holds it with 5 and 6 (sshd parity). */
#define AXL_SSH_MSG_EXT_INFO          7
/* 30 and 31 are kex-method-SPECIFIC numbers: the same two values mean
   different messages under a different kex. They are correct here only
   because exactly one kex is offered. */
#define AXL_SSH_MSG_KEX_ECDH_INIT    30
#define AXL_SSH_MSG_KEX_ECDH_REPLY   31
#define AXL_SSH_MSG_USERAUTH_REQUEST 50
#define AXL_SSH_MSG_USERAUTH_FAILURE 51
#define AXL_SSH_MSG_USERAUTH_SUCCESS 52
/* RFC 4252 section 5.4: may be sent any time after authentication starts and
   before it succeeds. */
#define AXL_SSH_MSG_USERAUTH_BANNER  53
/* 60 is METHOD-specific, the way 30 and 31 are kex-method-specific: under
   password it is SSH_MSG_USERAUTH_PASSWD_CHANGEREQ, and under
   keyboard-interactive it is SSH_MSG_USERAUTH_INFO_REQUEST. It is correct
   here even though password is now also accepted, because this constant is
   only ever sent as USERAUTH_PK_OK, from the publickey query response
   (send_pk_ok): the password path's CHANGE_FIRST notice goes out as
   USERAUTH_BANNER instead, so the two meanings of 60 never collide on the
   wire. */
#define AXL_SSH_MSG_USERAUTH_PK_OK   60

/* The whole user-authentication range (RFC 4250 section 4.1.2). Named as a
   RANGE because axl_ssh_unimplemented_due has to silence all of it after a
   successful login, not just USERAUTH_REQUEST. */
#define AXL_SSH_MSG_USERAUTH_FIRST   50
#define AXL_SSH_MSG_USERAUTH_LAST    79

#define AXL_SSH_MSG_GLOBAL_REQUEST   80
#define AXL_SSH_MSG_REQUEST_SUCCESS  81
#define AXL_SSH_MSG_REQUEST_FAILURE  82

#define AXL_SSH_MSG_CHANNEL_OPEN                90
#define AXL_SSH_MSG_CHANNEL_OPEN_CONFIRMATION   91
#define AXL_SSH_MSG_CHANNEL_OPEN_FAILURE        92
#define AXL_SSH_MSG_CHANNEL_WINDOW_ADJUST       93
#define AXL_SSH_MSG_CHANNEL_DATA                94
#define AXL_SSH_MSG_CHANNEL_EXTENDED_DATA       95
#define AXL_SSH_MSG_CHANNEL_EOF                 96
#define AXL_SSH_MSG_CHANNEL_CLOSE               97
#define AXL_SSH_MSG_CHANNEL_REQUEST             98
#define AXL_SSH_MSG_CHANNEL_SUCCESS             99
#define AXL_SSH_MSG_CHANNEL_FAILURE            100

/** RFC 4254 section 5.1 reason codes for CHANNEL_OPEN_FAILURE. */
#define AXL_SSH_OPEN_ADMINISTRATIVELY_PROHIBITED 1
#define AXL_SSH_OPEN_RESOURCE_SHORTAGE           4

/** RFC 4254 section 5.2 extended data type: stderr. */
#define AXL_SSH_EXTENDED_DATA_STDERR             1

/** RFC 4254 section 5.1 reason code: every channel type but `session`, and
    every type at all on a server with no channel hooks installed. */
#define AXL_SSH_OPEN_UNKNOWN_CHANNEL_TYPE   3

/* ---------------------------------------------------------------------------
 * RFC 4254 channel messages (sections 5 and 6)
 *
 * Pure codec: no connection state, no allocation beyond the builder's string.
 * Every parser takes bytes an authenticated-but-untrusted peer chose and
 * returns AXL_OK, or AXL_ERR for a NULL argument, a wrong message type,
 * truncation, or TRAILING bytes -- a trailing byte is a protocol error, as
 * axl_ssh_global_request_parse already treats it. The exceptions are the two
 * parsers whose tail is type-specific: axl_ssh_chan_open_parse and
 * axl_ssh_chan_req_parse hand it back unread as `rest`, for the request's
 * own parser -- or, for an open, the code that accepts its type -- to
 * check. Pointers handed out point
 * INTO the input. The whole-message parsers take the message INCLUDING its
 * type byte; the request-specific parsers take the CHANNEL_REQUEST `rest`.
 * ------------------------------------------------------------------------- */

/** CHANNEL_OPEN (90): string type, uint32 sender, window, max packet, then
    type-specific data left in @c rest unread -- x11 (RFC 4254 section
    6.3.2), direct-tcpip and forwarded-tcpip (section 7) all carry some, and
    a `session` (section 6.1) carries none. */
typedef struct {
    const uint8_t *type;
    const uint8_t *rest;
    size_t         rest_len;
    uint32_t       type_len;
    uint32_t       sender;
    uint32_t       window;
    uint32_t       max_packet;
} AxlSshChanOpen;

/** CHANNEL_REQUEST (98): uint32 recipient, string name, boolean want_reply,
    then request-specific fields left in @c rest unread. */
typedef struct {
    uint32_t       recipient;
    const uint8_t *name;
    uint32_t       name_len;
    bool           want_reply;
    const uint8_t *rest;
    size_t         rest_len;
} AxlSshChanReq;

/** Parse CHANNEL_OPEN up to the type-specific data, which is NOT checked
    for trailing bytes: what follows max packet depends on the type, and an
    open of a type the server refuses must be refused, not treated as a
    protocol error (sshd reads the same four fields and refuses an unknown
    type without touching the rest). The caller that ACCEPTS a type checks
    its data -- for `session`, that @c rest_len is 0.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_open_parse(const uint8_t *p, size_t len, AxlSshChanOpen *out);

/** Parse CHANNEL_REQUEST up to the request-specific data, which is NOT
    checked for trailing bytes (its parser does that). @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_req_parse(const uint8_t *p, size_t len, AxlSshChanReq *out);

/** Parse a "pty-req" @a rest: string TERM, uint32 cols, rows, width px,
    height px, string modes. The modes string is length-checked and ignored.
    TERM is copied NUL-terminated into @a term (@a term_cap bytes including
    the NUL). A NUL inside TERM is refused, as sshd does, except one as the
    very last byte, which sshd's sshbuf_get_cstring accepts. TERM longer than
    @a term_cap - 1 is refused.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_pty_parse(const uint8_t *rest, size_t len, char *term,
                           size_t term_cap, uint32_t dims[4]);

/** Parse a "window-change" @a rest: cols, rows, width px, height px.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_winch_parse(const uint8_t *rest, size_t len, uint32_t dims[4]);

/** Parse an "exec" @a rest: one string. Same NUL rule as TERM: a NUL inside
    the command is refused, one as the last byte is accepted and not counted
    in @a cmd_len. @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_exec_parse(const uint8_t *rest, size_t len,
                            const uint8_t **cmd, uint32_t *cmd_len);

/** Parse CHANNEL_EOF (96) or CHANNEL_CLOSE (97): type byte, uint32 recipient.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_u32_msg_parse(const uint8_t *p, size_t len, uint32_t *recipient);

/** Parse CHANNEL_WINDOW_ADJUST (93). @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_adjust_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                              uint32_t *bytes);

/** Parse CHANNEL_DATA (94): recipient, string data. @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_data_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                            const uint8_t **data, uint32_t *data_len);

/** Parse CHANNEL_EXTENDED_DATA (95): recipient, data type code, string data.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_ext_data_parse(const uint8_t *p, size_t len, uint32_t *recipient,
                                uint32_t *code, const uint8_t **data,
                                uint32_t *data_len);

/** Append CHANNEL_OPEN_CONFIRMATION (91). @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_open_confirm_build(AxlString *b, uint32_t recipient,
                                    uint32_t sender, uint32_t window,
                                    uint32_t max_packet);

/** Append CHANNEL_OPEN_FAILURE (92) with an empty description and language
    tag. @a reason is an AXL_SSH_OPEN_* code. @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_open_failure_build(AxlString *b, uint32_t recipient,
                                    uint32_t reason);

/** Append CHANNEL_DATA (94), or CHANNEL_EXTENDED_DATA (95) with the stderr
    code when @a stderr_. @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_data_build(AxlString *b, uint32_t recipient, bool stderr_,
                            const void *d, size_t n);

/** Append CHANNEL_WINDOW_ADJUST (93). @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_adjust_build(AxlString *b, uint32_t recipient, uint32_t bytes);

/** Append a type-plus-recipient message: EOF 96, CLOSE 97, SUCCESS 99 or
    FAILURE 100. Any other @a type is refused. @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_u32_msg_build(AxlString *b, uint8_t type, uint32_t recipient);

/** Append a CHANNEL_REQUEST "exit-status" with want_reply FALSE.
    @return AXL_OK or AXL_ERR. */
int axl_ssh_chan_exit_status_build(AxlString *b, uint32_t recipient,
                                   uint32_t status);

/** Append sshd's keepalive probe (serverloop.c client_alive_check): with
    @a on_channel a CHANNEL_REQUEST to @a recipient, otherwise a
    GLOBAL_REQUEST; either named "keepalive@openssh.com", want_reply TRUE,
    no request-specific data. @return AXL_OK or AXL_ERR. */
int axl_ssh_keepalive_build(AxlString *b, bool on_channel, uint32_t recipient);

/** Build SSH_MSG_UNIMPLEMENTED: byte 3, uint32 packet_sequence_number.

    @a seq is the sequence number of the message being answered -- the value
    BEFORE the receive counter was advanced for it, which is what the peer
    used and the only value it can match a message to.

    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_unimplemented_build(AxlString *out, uint32_t seq);

/** Does an unhandled message number earn an SSH_MSG_UNIMPLEMENTED reply?

    RFC 4253 section 11.4 makes the reply a MUST for an unrecognised message,
    and this is the list of messages that are recognised-but-unhandled or
    whose reply another rule forbids:

      - DISCONNECT, IGNORE and DEBUG are recognised. Section 11 says to
        ignore them, and a reply to a DISCONNECT would be a packet sent after
        the peer said it was leaving.
      - UNIMPLEMENTED itself, or two conforming peers answer each other for
        ever.
      - EVERY userauth message once @a post_auth: RFC 4252 section 5.1 has
        the server ignore authentication after success, and answering
        UNIMPLEMENTED is not ignoring. AxlSsh's own USERAUTH_REQUEST handler
        is unreachable in that state, so without this the rule would be
        broken by the very mechanism added to honour a different one.

    Callers are responsible for the per-connection reply cap; this decides
    only whether the message type earns one. Never called during key
    exchange, where strict KEX rule (a) ends the connection instead.

    @return true when a reply is owed. */
bool axl_ssh_unimplemented_due(uint8_t msg, bool post_auth);

/** Parse RFC 4254 section 4's SSH_MSG_GLOBAL_REQUEST: byte 80, string
    request name, boolean want_reply, then request-specific data that this
    parser deliberately does not read (AxlSsh implements no global request, so
    every one is refused whatever its data).

    A boolean is TRUE for any non-zero byte (RFC 4251 section 5).

    @return AXL_OK with *want_reply set; AXL_ERR if @a p is NULL, the
        message is not type 80, or it ends before the boolean -- a truncated
        request is a protocol error, as a truncated CHANNEL_OPEN already is. */
int axl_ssh_global_request_parse(const uint8_t *p, size_t len, bool *want_reply);

/** Append SSH_MSG_REQUEST_FAILURE (one byte, 82) to @a out.
    @return AXL_OK; AXL_ERR if @a out is NULL or the append fails. */
int axl_ssh_request_failure_build(AxlString *out);

/** A raw Ed25519 public key: the 32-byte compressed point of RFC 8032
    section 5.1.2, which is what axl_pk_key_from_raw_public accepts.

    Numerically equal to #AXL_SSH_X25519_LEN and semantically unrelated --
    a Montgomery u-coordinate and a compressed twisted-Edwards point are
    different objects that happen to be the same width. Spelling them apart
    is what stops a later edit to one silently resizing the other. */
#define AXL_SSH_ED25519_PUB_LEN  32u

/** An `ssh-ed25519` signature: R || S, RFC 8032 section 5.1.6. */
#define AXL_SSH_ED25519_SIG_LEN  64u

/** The kex hash is fixed at SHA-256 by curve25519-sha256, so this is a
    constant rather than a query. */
#define AXL_SSH_HASH_LEN   32u

/** An X25519 public key, and the shared secret it produces. */
#define AXL_SSH_X25519_LEN 32u

/** Build the `ssh-ed25519` public key blob: string("ssh-ed25519") followed by
    string(@a pub32). This is K_S -- it goes into the exchange hash and into
    KEX_ECDH_REPLY, both times wrapped in a further string.
    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_hostkey_blob(AxlString *out, const uint8_t *pub32);

/** Build the `ssh-ed25519` signature blob: string("ssh-ed25519") followed by
    string(@a sig64).
    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_signature_blob(AxlString *out, const uint8_t *sig64);

/** Parse an `ssh-ed25519` public key blob -- string("ssh-ed25519") followed
    by string(pub32) -- into its 32 raw bytes. The inverse of
    axl_ssh_hostkey_blob(), and role-neutral for the same reason: a server
    reads it out of a userauth request, a P6 client reads it out of
    KEX_ECDH_REPLY.

    Refuses trailing bytes inside the blob. A parser that stopped at the
    second string would let an attacker append anything to a blob whose
    prefix is a key we authorize.

    @return AXL_OK with @a out32 filled, or AXL_ERR on a NULL argument, a
    truncated blob, an inner type name that is not "ssh-ed25519", a key field
    that is not 32 bytes, or any byte left over. */
int axl_ssh_hostkey_blob_parse(const uint8_t *blob, size_t len,
                               uint8_t *out32);

/** Parse an `ssh-ed25519` signature blob into its 64 raw bytes. The inverse
    of axl_ssh_signature_blob(); same refusals.
    @return AXL_OK with @a out64 filled, or AXL_ERR. */
int axl_ssh_signature_blob_parse(const uint8_t *blob, size_t len,
                                 uint8_t *out64);

/** Build the bytes RFC 4252 section 7 signs for a publickey request:

      string(session_id) || <the request, from its message number through the
                             public key blob>

    The second half is passed in VERBATIM -- the bytes that arrived, never
    re-serialised from the fields just parsed out of them. A server that
    rebuilds them can differ from the client that signed them in any of six
    length-prefixed fields, and a canonicalisation difference in a signed blob
    is a signature bypass, not a formatting bug. Parsing the same bytes
    separately to learn the user name and key is safe because SSH strings are
    length-prefixed: there is no ambiguity for a second reading to resolve
    differently.

    Role-neutral: a P6 client signing a request it has just built hands its
    own bytes to this same function.

    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_userauth_signed_blob(AxlString *out,
                                 const uint8_t *session_id, size_t sid_len,
                                 const uint8_t *req, size_t req_len);

/** Compute the exchange hash H (RFC 4253 section 8, curve25519 flavour):

      H = SHA256( string(V_C) || string(V_S) || string(I_C) || string(I_S)
                  || string(K_S) || string(Q_C) || string(Q_S) || mpint(K) )

    @a v_c and @a v_s are the identification lines WITHOUT their CRLF, and
    @a i_c / @a i_s are the whole KEXINIT payloads INCLUDING the message
    number. Every one of those four is a place an implementation can be
    self-consistently wrong and interoperate with nothing, which is why the
    conformance test drives a real ssh(1).

    @return AXL_OK, or AXL_ERR on a NULL argument, allocation failure or a
    digest failure. */
int axl_ssh_exchange_hash(uint8_t *out, size_t out_len,
                          const char *v_c, const char *v_s,
                          const uint8_t *i_c, size_t i_c_len,
                          const uint8_t *i_s, size_t i_s_len,
                          const uint8_t *k_s, size_t k_s_len,
                          const uint8_t *q_c, size_t q_c_len,
                          const uint8_t *q_s, size_t q_s_len,
                          const uint8_t *k,   size_t k_len);

/* ---------------------------------------------------------------------------
 * chacha20-poly1305@openssh.com (PROTOCOL.chacha20poly1305)
 *
 * Not the RFC 8439 AEAD, and not interchangeable with axl_aead_seal(): this
 * construction encrypts the 4-byte packet_length field under a SEPARATE key so
 * a receiver can learn the length before authenticating, and it derives the
 * Poly1305 key from the payload cipher's counter-0 block. The sequence number
 * is the nonce; there is no explicit IV.
 * ------------------------------------------------------------------------- */

/** 512 bits of key material, as PROTOCOL.chacha20poly1305 requires. */
#define AXL_SSH_AEAD_KEY_LEN  64u

/** Poly1305 output, appended to every packet. */
#define AXL_SSH_AEAD_TAG_LEN  16u

/** Bytes of framing an AEAD packet adds around the payload: the encrypted
    4-byte length, the padding-length byte, and the tag. */
#define AXL_SSH_AEAD_OVERHEAD  (4u + 1u + AXL_SSH_AEAD_TAG_LEN)

/** One direction's cipher state. Two keys, not one: K_2 (first 32 bytes of
    the key material) encrypts the payload, K_1 (second 32) encrypts the
    length. OpenSSH's own ordering -- reversing them produces a cipher that
    talks to itself perfectly and to nothing else. */
typedef struct {
    uint8_t k_payload[32];  /**< K_2 -- payload, and the Poly1305 key. */
    uint8_t k_length[32];   /**< K_1 -- the packet_length field only. */
    bool    ready;          /**< false until a key is set. */
} AxlSshAead;

/** Load 64 bytes of key material into @a a.
    @return AXL_OK, or AXL_ERR on a NULL argument. */
int axl_ssh_aead_set_key(AxlSshAead *a, const uint8_t *key64);

/** Padding bytes for sealing, drawn from the CSPRNG in blocks rather than one
    firmware call per packet: axl_rng_bytes is a synchronous EFI_RNG_PROTOCOL
    call, measured at ~700 ns on x64/OVMF and ~18,170 ns on aa64/AAVMF -- 52%
    of a small outbound packet there, and the only per-packet cost whose share
    grows as the target slows. Never zero-filled: a refill failure fails the
    caller. */
#define AXL_SSH_RNG_POOL_LEN  256u

typedef struct {
    uint8_t  buf[AXL_SSH_RNG_POOL_LEN];
    uint16_t pos;       /**< next unused byte; == LEN means empty */
    uint32_t refills;   /**< firmware calls made -- what the unit test pins */
} AxlSshRngPool;

/** Initialise EMPTY (pos = LEN): no firmware call until the first take, so a
    connection that never seals costs nothing. */
void axl_ssh_rng_pool_init(AxlSshRngPool *p);

/** Copy @a n bytes (n <= AXL_SSH_RNG_POOL_LEN) into @a out, refilling the whole
    pool with ONE axl_rng_bytes call when fewer than @a n remain. Bytes are
    consumed, never reused; the unused tail of a pool is discarded at a refill
    rather than spliced (simpler, and costs at most n-1 bytes per refill).
    @return AXL_OK; AXL_ERR on NULL, n > LEN, or a failed refill (pool left
    empty, @a out untouched). */
int  axl_ssh_rng_pool_take(AxlSshRngPool *p, uint8_t *out, size_t n);

/** Scrub and empty. */
void axl_ssh_rng_pool_clear(AxlSshRngPool *p);

/** Frame, encrypt and authenticate @a payload, appending the whole packet to
    @a out. @a seq is the packet sequence number, which is the nonce -- so a
    caller that fails to advance it reuses a keystream. @a pad supplies the
    padding bytes (axl_ssh_rng_pool_take) instead of a direct axl_rng_bytes
    call.
    @return AXL_OK, or AXL_ERR on a NULL argument, a payload too large to
    frame, a pool refill failure, or allocation failure. */
int axl_ssh_aead_seal(const AxlSshAead *a, uint64_t seq,
                      const void *payload, size_t len,
                      AxlSshRngPool *pad, AxlString *out);

/* ---------------------------------------------------------------------------
 * Transmit queue (axl-ssh-txq.c)
 *
 * The outbound half of one connection, as bookkeeping only: no socket, no
 * loop, no firmware. The server keeps AT MOST ONE TCP send in flight per
 * connection, and everything sealed while that send is on the wire (or
 * before the deferred flush runs) waits here, to leave in the NEXT send as
 * one buffer. axl_tcp_send_async's completion is the expensive part of a
 * send, so paying it once per run instead of once per packet is the point.
 *
 * ORDER IS THE WHOLE CONTRACT. Frames leave in exactly the order they were
 * pushed, none dropped and none repeated: the sealed packets carry their
 * sequence number as the AEAD nonce, so a reordered frame is a bad MAC at the
 * peer, not a late one.
 * ------------------------------------------------------------------------- */

typedef struct {
    AxlString *pending;         /**< sealed frames not yet handed to TCP */
    unsigned   pending_frames;
    size_t     inflight_len;    /**< bytes of the one send on the wire, 0 = none */
    unsigned   inflight_frames;
} AxlSshTxq;

/** Initialise an empty queue.
    @return AXL_OK, or AXL_ERR on a NULL argument or allocation failure. */
int axl_ssh_txq_init(AxlSshTxq *q);

/** Release the pending run. The in-flight buffer is NOT the queue's -- it was
    handed to the caller by axl_ssh_txq_take -- so this frees only what is
    still pending. NULL-safe, and safe on a queue whose init failed. */
void axl_ssh_txq_free(AxlSshTxq *q);

/** Append one whole frame (an identification line or a sealed packet) to the
    pending run, after everything already pushed.
    @return AXL_OK, or AXL_ERR on a NULL argument, @a len 0 (an empty frame is
    a caller bug, and would count a frame that puts nothing on the wire), or
    allocation failure -- in which case the queue is unchanged. */
int axl_ssh_txq_push(AxlSshTxq *q, const void *frame, size_t len);

/** Nothing in flight and something pending: the moment a send may start. */
bool axl_ssh_txq_ready(const AxlSshTxq *q);

/** Hand the WHOLE pending run over for one send. On success @a *buf is a heap
    buffer the caller now owns (axl_free it after the send completes -- it is
    the send's borrowed buffer), @a *len and @a *frames describe it, pending
    is empty and the run is in flight.
    @return AXL_OK, or AXL_ERR on a NULL argument or when not ready (nothing
    pending, or a send already in flight) -- outputs untouched. */
int axl_ssh_txq_take(AxlSshTxq *q, uint8_t **buf, size_t *len,
                     unsigned *frames);

/** The in-flight send completed, successfully or not: nothing is in flight. */
void axl_ssh_txq_sent(AxlSshTxq *q);

/** Bytes queued, pending plus in flight: the figure an output high-water mark
    reads. */
size_t axl_ssh_txq_bytes(const AxlSshTxq *q);

/** Decrypt and verify one packet from @a p, writing the payload (message
    number included, padding excluded) into @a out.

    Checks the tag BEFORE trusting anything but the length, and bounds the
    length against #AXL_SSH_PACKET_MAX before allocating or copying -- the
    length field is the one value an attacker controls that is read before
    authentication.

    @return AXL_OK with *consumed set to the whole packet and *out_len to the
    payload size; AXL_INCOMPLETE when the packet has not fully arrived;
    AXL_ERR on a NULL argument, a length out of range, a padding length that
    does not fit, a tag mismatch, or @a out_cap too small. */
int axl_ssh_aead_open(const AxlSshAead *a, uint64_t seq,
                      const uint8_t *p, size_t len, size_t *consumed,
                      uint8_t *out, size_t out_cap, uint32_t *out_len);

/** Overwrite @a n bytes at @a p with zeros, unelidably.

    Deliberately not axl_memset: a compiler may remove a memset to storage
    that is dead afterwards, which is exactly the storage this is for. Shared
    rather than copied into each SSH translation unit -- three copies of a
    security primitive is three chances for one of them to be quietly
    replaced by memset. */
void axl_ssh_secure_zero(void *p, size_t n);

/** Scrub an AxlString's bytes, then free it. NULL-safe.

    axl_string_free() does NOT zeroize -- axl_free's fill is behind
    AXL_MEM_DEBUG, so in a RELEASE build the contents go back to the UEFI
    pool intact, and that pool is later handed to the OS loader and to other
    DXE drivers before ExitBootServices. Every buffer that has held the
    shared secret K, or a hash input containing it, goes through here.

    Not a general AxlString facility on purpose: the scrub belongs where the
    secret is known to be, and a "secure free" on the general string type
    would invite it being used as a substitute for thinking about which
    buffers hold key material. */
void axl_ssh_string_free_secure(AxlString *s);

/** The server's test seam: true makes the server act at @a point as a test
    asks. "tx-refuse" / "tx-refuse-linger": tx_flush reads the send as
    refused by TCP -- the second only on a connection in ST_LINGER, which is
    how a test fails the run carrying a DISCONNECT and nothing before it.
    "tx-complete-fail-linger": on_sent reads a send that COMPLETED on a
    connection in ST_LINGER as failed -- the transport losing bytes it had
    accepted, after the DISCONNECT was queued. The library's definition is WEAK and always false, so production carries
    no trace of a test control; ssh-server-selftest.c links a strong one
    (the _axl_service_stage_test_point pattern). Keep the `*_test_point`
    name: test-efi-pack-release-qemu.sh finds the seams in a shipped image
    by that suffix alone. */
bool _axl_ssh_server_test_point(const char *point);

#endif /* AXL_SSH_INTERNAL_H */
