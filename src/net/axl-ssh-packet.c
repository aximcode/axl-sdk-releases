/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-packet.c
    Binary packet protocol, RFC 4253 section 6. This is the UNENCRYPTED form
    used before NEWKEYS; the AEAD form arrives with key exchange.

    Both length fields on the wire are attacker-controlled and both are bounded
    before anything sizes a read from them. The unwrap side additionally
    enforces the two structural rules the RFC states and a lenient parser tends
    to skip -- a padding minimum and a block-multiple total -- because being
    lenient here means accepting frames a conforming peer never sends.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-rng.h>

int
axl_ssh_packet_wrap(AxlString *out, const void *payload, size_t len)
{
    /* Refuse before arithmetic: the wire length field is 32-bit, and a payload
       near SIZE_MAX would wrap it into a small, plausible-looking value. */
    if (out == NULL || (payload == NULL && len > 0) || len > AXL_SSH_PACKET_MAX) {
        return AXL_ERR;
    }

    /* Padding brings (4 + 1 + len + pad) to a multiple of the block size, and
       is never fewer than AXL_SSH_PAD_MIN bytes. */
    size_t unpadded = 4 + 1 + len;
    size_t pad = AXL_SSH_BLOCK - (unpadded % AXL_SSH_BLOCK);
    if (pad < AXL_SSH_PAD_MIN) {
        pad += AXL_SSH_BLOCK;
    }
    if (axl_ssh_put_u32(out, (uint32_t)(1 + len + pad)) != AXL_OK) {
        return AXL_ERR;
    }
    char pl = (char)(uint8_t)pad;
    if (axl_string_append_len(out, &pl, 1) != AXL_OK ||
        axl_string_append_len(out, (const char *)payload, len) != AXL_OK) {
        return AXL_ERR;
    }
    /* pad maxes out at AXL_SSH_BLOCK + (AXL_SSH_PAD_MIN - 1) = 11. */
    char padbuf[2 * AXL_SSH_BLOCK];
    if (axl_rng_bytes(padbuf, pad) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_string_append_len(out, padbuf, pad);
}

int
axl_ssh_packet_unwrap(const uint8_t *p, size_t len, size_t *consumed,
                      const uint8_t **payload, uint32_t *payload_len)
{
    if (p == NULL || consumed == NULL || payload == NULL || payload_len == NULL) {
        return AXL_ERR;
    }
    if (len < 4) {
        return AXL_INCOMPLETE;   /* not even the length prefix yet */
    }

    size_t off = 0;
    uint32_t plen = 0;
    if (axl_ssh_get_u32(p, len, &off, &plen) != AXL_OK) {
        return AXL_ERR;
    }
    /* Bound BEFORE trusting: plen is attacker-controlled and would otherwise
       size a read. The floor is padding_length(1) + the padding minimum.

       NOTE, so nobody deletes it as dead: the floor currently cannot be
       isolated by any input, and a sabotage of it goes undetected. The
       block-multiple rule below admits only plen == 4 from the sub-floor
       range, and the padding rules then reject that unconditionally
       (pad_len >= 4 makes pad_len + 1 > 4 always true). It is redundant only
       WHILE the pre-NEWKEYS block rule holds; AEAD framing does not carry that
       rule, so this becomes the load-bearing check the moment it lands. */
    if (plen < 1 + AXL_SSH_PAD_MIN || plen > AXL_SSH_PACKET_MAX) {
        return AXL_ERR;
    }
    /* The total must be a multiple of the block size. Checked against plen
       rather than against what arrived, so a malformed frame is rejected on the
       first 4 bytes instead of after we have waited for the rest of it. */
    if ((4 + plen) % AXL_SSH_BLOCK != 0) {
        return AXL_ERR;
    }
    if (len < 4 + plen) {
        return AXL_INCOMPLETE;
    }

    /* Named apart from wrap's `pad` so a sabotage can target this check alone;
       a shared name makes one sed disable both sites and muddies the verdict. */
    uint32_t pad_len = p[4];
    if (pad_len < AXL_SSH_PAD_MIN) {
        return AXL_ERR;
    }
    /* pad_len + 1 <= plen keeps the payload length below from underflowing. */
    if (pad_len + 1u > plen) {
        return AXL_ERR;
    }
    *payload     = p + 5;
    *payload_len = plen - 1 - pad_len;
    *consumed    = 4 + plen;
    return AXL_OK;
}

int
axl_ssh_unimplemented_build(AxlString *out, uint32_t seq)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    char hdr = (char)AXL_SSH_MSG_UNIMPLEMENTED;
    if (axl_string_append_len(out, &hdr, 1) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_ssh_put_u32(out, seq);
}

bool
axl_ssh_unimplemented_due(uint8_t msg, bool post_auth)
{
    switch (msg) {
    case AXL_SSH_MSG_DISCONNECT:
    case AXL_SSH_MSG_IGNORE:
    case AXL_SSH_MSG_UNIMPLEMENTED:
    case AXL_SSH_MSG_DEBUG:
        return false;
    default:
        break;
    }
    return !(post_auth
             && msg >= AXL_SSH_MSG_USERAUTH_FIRST
             && msg <= AXL_SSH_MSG_USERAUTH_LAST);
}

int
axl_ssh_global_request_parse(const uint8_t *p, size_t len, bool *want_reply)
{
    size_t         off  = 1;
    const uint8_t *name = NULL;
    uint32_t       nlen = 0;

    if (p == NULL || want_reply == NULL || len < 1
        || p[0] != AXL_SSH_MSG_GLOBAL_REQUEST
        || axl_ssh_get_string(p, len, &off, &name, &nlen) != AXL_OK
        || off >= len) {
        return AXL_ERR;
    }
    *want_reply = p[off] != 0;
    return AXL_OK;
}

int
axl_ssh_request_failure_build(AxlString *out)
{
    char hdr = (char)AXL_SSH_MSG_REQUEST_FAILURE;

    if (out == NULL) {
        return AXL_ERR;
    }
    return axl_string_append_len(out, &hdr, 1);
}
