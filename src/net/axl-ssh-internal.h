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
 * The AEAD form arrives in Task 6.
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

/** Build our KEXINIT payload: message number, 16-byte cookie, ten name-lists,
    first_kex_packet_follows and the reserved uint32.
    @return AXL_OK, or AXL_ERR on allocation or RNG failure. */
int axl_ssh_kexinit_build(AxlString *out);

/** Check the peer's KEXINIT offers our single choice in every slot we use.
    Refuses rather than negotiates: there is one name per slot by design, and
    an algorithm matrix is a downgrade surface.
    @return AXL_OK when every required slot names our choice; AXL_ERR on a NULL
    argument, a malformed message, or any slot that does not. */
int axl_ssh_kexinit_select(const uint8_t *p, size_t len);

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

#endif /* AXL_SSH_INTERNAL_H */
