/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-rekey.c
    Key re-exchange: the pure decisions axl-ssh-rekey.h declares.

    Each rule cites the OpenSSH 9.9p1 function it reproduces; the header
    lists them together. Where sshd and RFC 4253 section 7.1 differ -- sshd
    keeps processing channel messages after the peer's KEXINIT, which the
    RFC says a compliant peer never sends -- this follows sshd.
**/

#include "axl-ssh-rekey.h"

/* RFC 4250 section 4.1.2: the transport layer owns 1-49, and 30-49 belong
   to whichever key-exchange method was negotiated. */
#define AXL_SSH_REKEY_TRANSPORT_LAST  49u
#define AXL_SSH_REKEY_METHOD_FIRST    30u

/* ------------------------------------------------------------------------ */
/* Triggers and counters                                                     */
/* ------------------------------------------------------------------------ */

void
axl_ssh_rekey_init(AxlSshRekey *r, uint64_t limit_bytes)
{
    if (r == NULL) {
        return;
    }
    axl_memset(r, 0, sizeof *r);
    r->limit_bytes = limit_bytes;
}

void
axl_ssh_rekey_count_out(AxlSshRekey *r, size_t bytes)
{
    if (r == NULL) {
        return;
    }
    r->bytes_out += bytes;
    r->pkts_out++;
}

void
axl_ssh_rekey_count_in(AxlSshRekey *r, size_t bytes)
{
    if (r == NULL) {
        return;
    }
    r->bytes_in += bytes;
    r->pkts_in++;
}

bool
axl_ssh_rekey_active(const AxlSshRekey *r)
{
    return r != NULL && (r->ours_sent || r->peers_seen);
}

bool
axl_ssh_rekey_due(const AxlSshRekey *r)
{
    /* sshd returns early on "KEX in progress" before any limit is read, so
       a limit crossed mid-exchange waits for the next one rather than
       nesting a second exchange inside the first. */
    if (r == NULL || axl_ssh_rekey_active(r)) {
        return false;
    }
    if (r->pkts_out > AXL_SSH_REKEY_PACKETS || r->pkts_in > AXL_SSH_REKEY_PACKETS) {
        return true;
    }
    return r->limit_bytes != 0
           && (r->bytes_out > r->limit_bytes || r->bytes_in > r->limit_bytes);
}

/* ------------------------------------------------------------------------ */
/* Message policy                                                            */
/* ------------------------------------------------------------------------ */

/* sshd's ssh_packet_type_is_kex(): the transport range minus the two the RFC
   forbids mid-exchange and EXT_INFO, which sshd holds with them. */
bool
axl_ssh_rekey_is_kex(uint8_t msg)
{
    return msg >= AXL_SSH_MSG_DISCONNECT && msg <= AXL_SSH_REKEY_TRANSPORT_LAST
           && msg != AXL_SSH_MSG_SERVICE_REQUEST
           && msg != AXL_SSH_MSG_SERVICE_ACCEPT
           && msg != AXL_SSH_MSG_EXT_INFO;
}

AxlSshRekeyOut
axl_ssh_rekey_out(const AxlSshRekey *r, uint8_t msg)
{
    if (r == NULL || !r->ours_sent || r->ours_newkeys || axl_ssh_rekey_is_kex(msg)) {
        return AXL_SSH_REKEY_SEND;
    }
    return AXL_SSH_REKEY_HOLD;
}

bool
axl_ssh_rekey_in_allowed(const AxlSshRekey *r, uint8_t msg)
{
    if (r == NULL) {
        return false;
    }
    if (!r->peers_seen) {
        return true;
    }
    /* The rest mirrors sshd's dispatch table between the peer's KEXINIT
       and its NEWKEYS; the header gives the function behind each line. */
    if (msg >= AXL_SSH_MSG_DISCONNECT && msg <= AXL_SSH_MSG_DEBUG) {
        return true;
    }
    if (msg == AXL_SSH_MSG_NEWKEYS) {
        return r->ours_newkeys;
    }
    if (msg >= AXL_SSH_REKEY_METHOD_FIRST) {
        /* 30-49 are the exchange's own; 50 and up keep their handlers. */
        return true;
    }
    return false;
}

bool
axl_ssh_rekey_in_kex(const AxlSshRekey *r)
{
    /* The peer's half ends the exchange (its NEWKEYS can only come after
       ours), so "peer mid-exchange" is exactly peers_seen. */
    return r != NULL && r->peers_seen;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

/* Every event refuses an order sshd never reaches and then changes nothing:
   a flag set by a stray event would outlive its exchange and corrupt the
   next one's out() and in_allowed() answers. */

int
axl_ssh_rekey_on_our_kexinit(AxlSshRekey *r)
{
    if (r == NULL || r->ours_sent) {
        return AXL_ERR;
    }
    r->ours_sent = true;
    return AXL_OK;
}

int
axl_ssh_rekey_on_peer_kexinit(AxlSshRekey *r, bool *send_ours)
{
    if (r == NULL || r->peers_seen) {
        return AXL_ERR;
    }
    r->peers_seen = true;
    /* sshd's kex_input_kexinit answers with its own unless already sent. */
    if (send_ours != NULL) {
        *send_ours = !r->ours_sent;
    }
    return AXL_OK;
}

int
axl_ssh_rekey_on_our_newkeys(AxlSshRekey *r)
{
    if (r == NULL || !r->ours_sent || !r->peers_seen || r->ours_newkeys) {
        return AXL_ERR;
    }
    /* sshd's ssh_set_newkeys(MODE_OUT): what the OLD key carried no longer
       counts against the new one. */
    r->bytes_out    = 0;
    r->pkts_out     = 0;
    r->ours_newkeys = true;
    return AXL_OK;
}

int
axl_ssh_rekey_on_peer_newkeys(AxlSshRekey *r)
{
    /* ours_newkeys is the whole precondition: on_our_newkeys sets it only
       with both KEXINITs in, and only this function clears it. A separate
       peers_seen test here could never fail, so it is not written. */
    if (r == NULL || !r->ours_newkeys) {
        return AXL_ERR;
    }
    /* ssh_set_newkeys(MODE_IN), and the exchange is over: every flag back
       to "none running" so the next trigger can start one. */
    r->bytes_in     = 0;
    r->pkts_in      = 0;
    r->ours_sent    = false;
    r->peers_seen   = false;
    r->ours_newkeys = false;
    return AXL_OK;
}

/* ------------------------------------------------------------------------ */
/* Staged keys                                                               */
/* ------------------------------------------------------------------------ */

int
axl_ssh_dirkey_stage(AxlSshDirKey *k, const uint8_t key[AXL_SSH_AEAD_KEY_LEN])
{
    if (k == NULL || key == NULL || k->has_next) {
        return AXL_ERR;
    }
    if (axl_ssh_aead_set_key(&k->next, key) != AXL_OK) {
        return AXL_ERR;
    }
    k->has_next = true;
    return AXL_OK;
}

int
axl_ssh_dirkey_switch(AxlSshDirKey *k)
{
    if (k == NULL || !k->has_next) {
        return AXL_ERR;
    }
    /* Scrub before the copy rather than relying on the copy to overwrite:
       the scrub is unelidable, and it does not depend on the two structs
       staying the same shape. Then scrub the staged duplicate, so the new
       key lives in exactly one place. */
    axl_ssh_secure_zero(&k->cur, sizeof k->cur);
    axl_memcpy(&k->cur, &k->next, sizeof k->cur);
    axl_ssh_secure_zero(&k->next, sizeof k->next);
    k->has_next = false;
    return AXL_OK;
}

void
axl_ssh_dirkey_clear(AxlSshDirKey *k)
{
    if (k != NULL) {
        axl_ssh_secure_zero(k, sizeof *k);
    }
}
