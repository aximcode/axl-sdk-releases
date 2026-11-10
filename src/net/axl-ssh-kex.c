/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-kex.c
    KEXINIT and algorithm selection. ONE name per slot by design: negotiation
    is a downgrade surface, and we have no legacy peers to accommodate.

    The peer still sends LISTS, so each slot is a set-membership test, not an
    equality test -- a stock ssh(1) offers a dozen kex names and expects the
    server to pick. What we refuse is a peer that does not offer ours at all.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-digest.h>
#include <axl/axl-rng.h>
#include <axl/axl-ssh-core.h>
#include <stdbool.h>

#define KEX_NAME   "curve25519-sha256"
/* The kex slot's WHOLE list: the real method, then OpenSSH's strict-KEX
   pseudo-algorithm (PROTOCOL section 1.10). Spelled out here rather than
   concatenated inside axl_ssh_kexinit_build's initialiser, where two
   adjacent literals in a ten-element array of strings is exactly the shape
   clang-tidy's bugprone-suspicious-missing-comma exists to flag -- and it is
   right to: in that position a reader cannot tell a deliberate
   concatenation from a dropped comma. */
static const char KEX_LIST[] = KEX_NAME "," AXL_SSH_KEX_STRICT_S;
#define HOST_NAME  "ssh-ed25519"
#define CIPH_NAME  "chacha20-poly1305@openssh.com"
#define MAC_NAME   ""        /* implicit in the AEAD */
#define COMP_NAME  "none"

void
axl_ssh_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

/* Slot indices within the ten name-lists (RFC 4253 section 7.1). */
enum {
    SLOT_KEX       = 0,
    SLOT_HOSTKEY   = 1,
    SLOT_CIPH_C2S  = 2,
    SLOT_CIPH_S2C  = 3,
    SLOT_MAC_C2S   = 4,
    SLOT_MAC_S2C   = 5,
    SLOT_COMP_C2S  = 6,
    SLOT_COMP_S2C  = 7,
    SLOT_LANG_C2S  = 8,
    SLOT_LANG_S2C  = 9
};

int
axl_ssh_kexinit_build(AxlString *out, bool initial)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    char hdr = (char)AXL_SSH_MSG_KEXINIT;
    char cookie[16];
    if (axl_string_append_len(out, &hdr, 1) != AXL_OK ||
        axl_rng_bytes(cookie, sizeof cookie) != AXL_OK ||
        axl_string_append_len(out, cookie, sizeof cookie) != AXL_OK) {
        return AXL_ERR;
    }
    /* The kex slot is the ONE list that is not a single name: OpenSSH's
       strict-KEX pseudo-algorithm (PROTOCOL section 1.10) rides there, after
       the real method. Order matters twice -- RFC 4253 section 7.1 makes the
       FIRST name our guess, and a peer reads it to decide what we proposed,
       so KEX_NAME stays in front -- and the extra name is announced ONLY
       here, in the built offer. The selector's want[] must keep requiring
       the real method; see AXL_SSH_KEX_STRICT_S.

       ONLY in the INITIAL KEXINIT. A rekey's offers the real method alone,
       and the server ignores the peer's pseudo-name there too: section 1.10
       says they are "only valid in the initial SSH2_MSG_KEXINIT", and strict
       KEX, once agreed, stays in force for the life of the connection. */
    const char *const lists[AXL_SSH_KEXINIT_LISTS] = {
        initial ? KEX_LIST : KEX_NAME, HOST_NAME, CIPH_NAME, CIPH_NAME,
        MAC_NAME, MAC_NAME, COMP_NAME, COMP_NAME, "", ""
    };
    for (int i = 0; i < AXL_SSH_KEXINIT_LISTS; i++) {
        if (axl_ssh_put_string(out, lists[i], axl_strlen(lists[i])) != AXL_OK) {
            return AXL_ERR;
        }
    }
    char tail[5] = { 0, 0, 0, 0, 0 };   /* first_kex_packet_follows + reserved */
    return axl_string_append_len(out, tail, sizeof tail);
}

/* True when `name` is the FIRST entry of the comma-separated name-list
   [p, p+n). Distinct from membership: RFC 4253 section 7.1's "guess" is for
   the peer's first choice in each slot, so `curve25519-sha256@libssh.org`
   must NOT match `curve25519-sha256` -- which is what the trailing-comma
   test enforces. */
static bool
name_list_first_is(const uint8_t *p, uint32_t n, const char *name)
{
    size_t want = axl_strlen(name);
    return n >= want
           && axl_memcmp(p, name, want) == 0
           && (n == want || p[want] == ',');
}

/* True when `name` appears in the comma-separated name-list [p, p+n). */
static bool
name_list_has(const uint8_t *p, uint32_t n, const char *name)
{
    size_t want = axl_strlen(name);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || p[i] == ',') {
            if ((size_t)(i - start) == want &&
                (want == 0 || axl_memcmp(p + start, name, want) == 0)) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

int
axl_ssh_kexinit_select(const uint8_t *p, size_t len, AxlSshKexinitPeer *out)
{
    if (out != NULL) {
        out->guessed_wrong = false;
        out->strict_kex    = false;
    }
    /* 1 message number + 16 cookie. */
    if (p == NULL || len < 17 || p[0] != AXL_SSH_MSG_KEXINIT) {
        return AXL_ERR;
    }

    /* What each slot must offer. NULL means "we do not care what is here, but
       it must still be a well-formed name-list": the MAC slots are unused
       because the cipher is an AEAD, and languages are advisory. */
    static const char *const want[AXL_SSH_KEXINIT_LISTS] = {
        [SLOT_KEX]      = KEX_NAME,
        [SLOT_HOSTKEY]  = HOST_NAME,
        [SLOT_CIPH_C2S] = CIPH_NAME,
        [SLOT_CIPH_S2C] = CIPH_NAME,
        [SLOT_MAC_C2S]  = NULL,
        [SLOT_MAC_S2C]  = NULL,
        /* Compression is a non-goal, so "none" must be on offer. Without this
           a peer that only offers zlib is accepted here and fails later, at a
           point where the failure no longer names its cause. */
        [SLOT_COMP_C2S] = COMP_NAME,
        [SLOT_COMP_S2C] = COMP_NAME,
        [SLOT_LANG_C2S] = NULL,
        [SLOT_LANG_S2C] = NULL
    };

    /* Walk ALL ten lists, not only the interesting ones: a message that stops
       early is malformed, and accepting it would mean we validate only what we
       happen to read. */
    size_t off = 17;
    bool   kex_guess_is_ours     = false;
    bool   hostkey_guess_is_ours = false;
    bool   strict_kex            = false;
    for (int i = 0; i < AXL_SSH_KEXINIT_LISTS; i++) {
        const uint8_t *s = NULL;
        uint32_t n = 0;
        if (axl_ssh_get_string(p, len, &off, &s, &n) != AXL_OK) {
            return AXL_ERR;
        }
        if (want[i] != NULL && !name_list_has(s, n, want[i])) {
            return AXL_ERR;
        }
        /* RFC 4253 section 7.1: a peer's GUESS is for the FIRST name in its
           own list, not for any name it happens to offer -- so this is a
           prefix test, not the membership test above.

           BOTH slots matter. The RFC says the guess is wrong when "the kex
           algorithm AND/OR the host key algorithm is guessed wrong", and
           checking only the kex leaves a peer whose first host key name is
           not ours undetected: its guessed packet is then dispatched as a
           real message, which is exactly the failure this check exists to
           prevent. */
        if (i == SLOT_KEX) {
            kex_guess_is_ours = name_list_first_is(s, n, KEX_NAME);
            /* OpenSSH PROTOCOL section 1.10: the peer asks for strict KEX by
               carrying its own pseudo-name here. Membership, not position --
               ssh(1) sends it last, after ext-info-c. Held in a LOCAL and
               published only on the AXL_OK path below: a later slot can
               still refuse the message, and a caller must never read a
               strict_kex taken from a KEXINIT we rejected. */
            strict_kex = name_list_has(s, n, AXL_SSH_KEX_STRICT_C);
        } else if (i == SLOT_HOSTKEY) {
            hostkey_guess_is_ours = name_list_first_is(s, n, HOST_NAME);
        }
    }
    /* first_kex_packet_follows (1) + reserved (4). */
    if (len - off < 5) {
        return AXL_ERR;
    }
    /* The flag is READ, not merely stepped over. A peer that set it and
       guessed a different kex has already sent a packet for that method; it
       must be dropped rather than dispatched, or the connection dies on a
       message the RFC says to ignore. */
    if (out != NULL) {
        out->guessed_wrong = p[off] != 0
                             && !(kex_guess_is_ours && hostkey_guess_is_ours);
        out->strict_kex    = strict_kex;
    }
    return AXL_OK;
}

/* Feed one hash block's worth of the common prefix: K || H. Both derivations
   below start with it, and writing it once keeps the two from drifting -- a
   prefix mismatch between K1 and Kn+1 is exactly the self-consistent bug that
   interoperates with nothing. */
static void
kdf_prefix(AxlChecksum *d, const uint8_t *k, size_t k_len,
           const uint8_t *h, size_t h_len)
{
    axl_checksum_update(d, k, k_len);
    axl_checksum_update(d, h, h_len);
}

int
axl_ssh_kdf(const uint8_t *k, size_t k_len,
            const uint8_t *h, size_t h_len,
            char letter, const uint8_t *session_id, size_t sid_len,
            uint8_t *out, size_t out_len)
{
    if (k == NULL || h == NULL || session_id == NULL || out == NULL) {
        return AXL_ERR;
    }

    AxlChecksum *d = axl_checksum_new(AXL_CHECKSUM_SHA256);
    if (d == NULL) {
        return AXL_ERR;
    }

    uint8_t block[AXL_SSH_HASH_LEN];
    size_t blen = sizeof block;
    size_t produced = 0;

    /* K1 = HASH(K || H || letter || session_id) */
    kdf_prefix(d, k, k_len, h, h_len);
    axl_checksum_update(d, &letter, 1);
    axl_checksum_update(d, session_id, sid_len);
    axl_checksum_get_digest(d, block, &blen);
    if (blen != sizeof block) {
        axl_ssh_secure_zero(block, sizeof block);
        axl_checksum_free(d);
        return AXL_ERR;
    }

    while (produced < out_len) {
        size_t n = out_len - produced;
        if (n > sizeof block) {
            n = sizeof block;
        }
        axl_memcpy(out + produced, block, n);
        produced += n;
        if (produced < out_len) {
            /* Kn+1 = HASH(K || H || K1 || ... || Kn) -- the whole output so
               far, not just the previous block. */
            axl_checksum_reset(d);
            kdf_prefix(d, k, k_len, h, h_len);
            axl_checksum_update(d, out, produced);
            blen = sizeof block;
            axl_checksum_get_digest(d, block, &blen);
            if (blen != sizeof block) {
                axl_ssh_secure_zero(block, sizeof block);
                axl_checksum_free(d);
                return AXL_ERR;
            }
        }
    }
    axl_checksum_free(d);
    /* `block` still holds the last derived key block. */
    axl_ssh_secure_zero(block, sizeof block);
    return AXL_OK;
}

/* ---------------------------------------------------------------------------
 * Curve25519 exchange: mpint, the key blobs, and the exchange hash.
 * ------------------------------------------------------------------------- */

void
axl_ssh_string_free_secure(AxlString *s)
{
    if (s != NULL) {
        axl_ssh_secure_zero((void *)axl_string_data(s), axl_string_len(s));
        axl_string_free(s);
    }
}

int
axl_ssh_put_mpint(AxlString *b, const uint8_t *v, size_t n)
{
    if (b == NULL || (v == NULL && n != 0)) {
        return AXL_ERR;
    }
    /* Strip leading zero bytes: an mpint is MINIMAL, and a producer that
       emits the fixed-width magnitude instead builds a hash input that
       differs from every peer's for one shared secret in 256. */
    size_t i = 0;
    while (i < n && v[i] == 0) {
        i++;
    }
    size_t mag = n - i;
    if (mag == 0) {
        return axl_ssh_put_string(b, "", 0);
    }
    /* Two's complement: a top bit set would read as negative, so a zero byte
       goes in front. This is the branch that fires for about half of all
       X25519 shared secrets, and getting it wrong is invisible until a real
       client rejects the exchange hash. */
    if ((v[i] & 0x80) != 0) {
        if (axl_ssh_put_u32(b, (uint32_t)(mag + 1)) != AXL_OK) {
            return AXL_ERR;
        }
        char zero = 0;
        if (axl_string_append_len(b, &zero, 1) != AXL_OK) {
            return AXL_ERR;
        }
        return axl_string_append_len(b, (const char *)(v + i), mag);
    }
    return axl_ssh_put_string(b, v + i, mag);
}

int
axl_ssh_hostkey_blob(AxlString *out, const uint8_t *pub32)
{
    if (out == NULL || pub32 == NULL) {
        return AXL_ERR;
    }
    if (axl_ssh_put_string(out, HOST_NAME, axl_strlen(HOST_NAME)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ssh_put_string(out, pub32, 32);
}

int
axl_ssh_signature_blob(AxlString *out, const uint8_t *sig64)
{
    if (out == NULL || sig64 == NULL) {
        return AXL_ERR;
    }
    if (axl_ssh_put_string(out, HOST_NAME, axl_strlen(HOST_NAME)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ssh_put_string(out, sig64, 64);
}

/* Both parsers are the same shape -- string(name) || string(fixed-width) with
   nothing left over -- so the shape lives here once and each public parser
   supplies its expected width. */
static int
blob_parse_fixed(const uint8_t *blob, size_t len, uint8_t *out,
                 uint32_t out_len)
{
    if (blob == NULL || out == NULL) {
        return AXL_ERR;
    }
    size_t         off      = 0;
    const uint8_t *name     = NULL;
    uint32_t       name_len = 0;
    const uint8_t *body     = NULL;
    uint32_t       body_len = 0;
    if (axl_ssh_get_string(blob, len, &off, &name, &name_len) != AXL_OK
        || name_len != sizeof AXL_SSH_HOSTKEY_NAME - 1
        || axl_memcmp(name, AXL_SSH_HOSTKEY_NAME, name_len) != 0
        || axl_ssh_get_string(blob, len, &off, &body, &body_len) != AXL_OK
        || body_len != out_len
        || off != len) {
        return AXL_ERR;
    }
    axl_memcpy(out, body, out_len);
    return AXL_OK;
}

int
axl_ssh_hostkey_blob_parse(const uint8_t *blob, size_t len, uint8_t *out32)
{
    return blob_parse_fixed(blob, len, out32, AXL_SSH_ED25519_PUB_LEN);
}

int
axl_ssh_signature_blob_parse(const uint8_t *blob, size_t len, uint8_t *out64)
{
    return blob_parse_fixed(blob, len, out64, AXL_SSH_ED25519_SIG_LEN);
}

int
axl_ssh_exchange_hash(uint8_t *out, size_t out_len,
                      const char *v_c, const char *v_s,
                      const uint8_t *i_c, size_t i_c_len,
                      const uint8_t *i_s, size_t i_s_len,
                      const uint8_t *k_s, size_t k_s_len,
                      const uint8_t *q_c, size_t q_c_len,
                      const uint8_t *q_s, size_t q_s_len,
                      const uint8_t *k,   size_t k_len)
{
    if (out == NULL || out_len < AXL_SSH_HASH_LEN || v_c == NULL
        || v_s == NULL || i_c == NULL || i_s == NULL || k_s == NULL
        || q_c == NULL || q_s == NULL || k == NULL) {
        return AXL_ERR;
    }

    /* Assembled into one buffer rather than streamed into the digest, because
       every field is length-prefixed and building it here lets put_string and
       put_mpint own the encoding -- the same two functions the wire uses, so
       the hash and the packet cannot disagree about how a field is framed. */
    AxlString *b = axl_string_new("");
    if (b == NULL) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (axl_ssh_put_string(b, v_c, axl_strlen(v_c)) == AXL_OK
        && axl_ssh_put_string(b, v_s, axl_strlen(v_s)) == AXL_OK
        && axl_ssh_put_string(b, i_c, i_c_len) == AXL_OK
        && axl_ssh_put_string(b, i_s, i_s_len) == AXL_OK
        && axl_ssh_put_string(b, k_s, k_s_len) == AXL_OK
        && axl_ssh_put_string(b, q_c, q_c_len) == AXL_OK
        && axl_ssh_put_string(b, q_s, q_s_len) == AXL_OK
        && axl_ssh_put_mpint(b, k, k_len) == AXL_OK) {
        size_t n = AXL_SSH_HASH_LEN;
        rc = axl_compute_checksum_digest(AXL_CHECKSUM_SHA256,
                                         axl_string_data(b),
                                         axl_string_len(b), out, n);
    }
    /* `b` ends with mpint(K) -- the shared secret, in a heap buffer that is
       about to go back to the UEFI pool. */
    axl_ssh_string_free_secure(b);
    return rc;
}
