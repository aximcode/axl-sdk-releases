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
#include <stdbool.h>

/** SHA-256 digest length. The kex hash is fixed at SHA-256 by
    curve25519-sha256, so this is a constant rather than a query. */
#define AXL_SSH_HASH_LEN  32u

#define KEX_NAME   "curve25519-sha256"
#define HOST_NAME  "ssh-ed25519"
#define CIPH_NAME  "chacha20-poly1305@openssh.com"
#define MAC_NAME   ""        /* implicit in the AEAD */
#define COMP_NAME  "none"

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
axl_ssh_kexinit_build(AxlString *out)
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
    static const char *const lists[AXL_SSH_KEXINIT_LISTS] = {
        KEX_NAME, HOST_NAME, CIPH_NAME, CIPH_NAME, MAC_NAME,
        MAC_NAME, COMP_NAME, COMP_NAME, "", ""
    };
    for (int i = 0; i < AXL_SSH_KEXINIT_LISTS; i++) {
        if (axl_ssh_put_string(out, lists[i], axl_strlen(lists[i])) != AXL_OK) {
            return AXL_ERR;
        }
    }
    char tail[5] = { 0, 0, 0, 0, 0 };   /* first_kex_packet_follows + reserved */
    return axl_string_append_len(out, tail, sizeof tail);
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
axl_ssh_kexinit_select(const uint8_t *p, size_t len)
{
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
    for (int i = 0; i < AXL_SSH_KEXINIT_LISTS; i++) {
        const uint8_t *s = NULL;
        uint32_t n = 0;
        if (axl_ssh_get_string(p, len, &off, &s, &n) != AXL_OK) {
            return AXL_ERR;
        }
        if (want[i] != NULL && !name_list_has(s, n, want[i])) {
            return AXL_ERR;
        }
    }
    /* first_kex_packet_follows (1) + reserved (4). */
    if (len - off < 5) {
        return AXL_ERR;
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
                axl_checksum_free(d);
                return AXL_ERR;
            }
        }
    }
    axl_checksum_free(d);
    return AXL_OK;
}
