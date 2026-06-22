/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tpm-seal.c
    TPM2 PCR-bound seal / unseal over EFI_TCG2_PROTOCOL.SubmitCommand.

    Seal chains TPM2_CreatePrimary (a deterministic ECC SRK in the owner
    hierarchy) -> TPM2_PCR_Read (compute the PolicyPCR digest in software)
    -> TPM2_Create (a keyedhash sealed-data object whose authPolicy is
    that digest). The opaque blob the caller persists is the sealed
    object's {public, private} plus the PCR selection.

    Unseal chains the same CreatePrimary -> TPM2_Load -> TPM2_StartAuthSession
    (policy) -> TPM2_PolicyPCR (the TPM recomputes the digest from the LIVE
    PCRs) -> TPM2_Unseal. The TPM only releases the secret when the live
    PCRs reproduce the seal-time policy digest.

    All commands use the empty-password owner/parent auth (works on a TPM
    whose owner authorization is empty — the common firmware default;
    cross-boot unseal additionally needs the owner primary seed to be
    stable, i.e. no TPM2_Clear between seal and unseal).
**/

#include "../backend/axl-backend.h"   /* axl_bs, axl_efi_call, EFI types */
#include <uefi/axl-uefi.h>            /* EFI_TCG2_PROTOCOL (extra) */
#include "axl-tpm-internal.h"         /* TpmWr/TpmRd cursors + shared constants */
#include <axl/axl-tpm.h>
#include <axl/axl-digest.h>          /* axl_compute_checksum_digest (SHA-256) */
#include <axl/axl-rng.h>             /* axl_rng_bytes */
#include <axl/axl-mem.h>             /* axl_malloc / axl_free */
#include <axl/axl-str.h>             /* axl_memcpy / axl_memset */

// ===================================================================
// Seal-specific TPM2 constants
// ===================================================================

#define TPM_CC_CREATE_PRIMARY      0x00000131u
#define TPM_CC_CREATE              0x00000153u
#define TPM_CC_LOAD                0x00000157u
#define TPM_CC_UNSEAL              0x0000015Eu
#define TPM_CC_START_AUTH_SESSION  0x00000176u
#define TPM_CC_PCR_READ            0x0000017Eu
#define TPM_CC_POLICY_PCR          0x0000017Fu
#define TPM_RH_OWNER               0x40000001u
#define TPM_RH_NULL                0x40000007u
#define TPM_SE_POLICY              0x01u
#define TPM_ALG_KEYEDHASH          0x0008u

/* SRK: fixedTPM|fixedParent|sensitiveDataOrigin|userWithAuth|noDA|
   restricted|decrypt (TCG standard storage primary). */
#define SRK_ATTRIBUTES             0x00030472u
/* Sealed keyedhash object: fixedTPM|fixedParent only — no userWithAuth, so
   it can be used only by satisfying its authPolicy. */
#define SEAL_ATTRIBUTES            0x00000012u

#define MAX_PCRS  8

// ===================================================================
// TCG2 protocol + command submission
// ===================================================================

/* EFI_TCG2_PROTOCOL_GUID (TCG EFI Protocol spec). */
static const EFI_GUID TCG2_GUID = {
    0x607f766c, 0x7455, 0x42be,
    { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f }
};

static EFI_TCG2_PROTOCOL *
seal_tcg2(void)
{
    EFI_TCG2_PROTOCOL *p = NULL;
    EFI_STATUS st = axl_efi_call(
        axl_bs()->LocateProtocol, 3, (EFI_GUID *)&TCG2_GUID, NULL, (void **)&p);
    return EFI_ERROR(st) ? NULL : p;
}

static void
patch_size(uint8_t *cmd, size_t pos, size_t total)
{
    cmd[pos + 0] = (uint8_t)(total >> 24);
    cmd[pos + 1] = (uint8_t)(total >> 16);
    cmd[pos + 2] = (uint8_t)(total >> 8);
    cmd[pos + 3] = (uint8_t)total;
}

/* Submit @p cmd; on a transport-level success position @p out_rd just past
   the 10-byte response header (with len = responseSize) and return the TPM
   responseCode (0 == success). Returns ~0u on an EFI/parse error. */
static uint32_t
tpm_submit(EFI_TCG2_PROTOCOL *p, const uint8_t *cmd, size_t cmd_len,
           uint8_t *resp, size_t resp_cap, TpmRd *out_rd)
{
    axl_memset(resp, 0, resp_cap);
    EFI_STATUS st = axl_efi_call(p->SubmitCommand, 5, p,
                                 (UINT32)cmd_len, (uint8_t *)cmd,
                                 (UINT32)resp_cap, resp);
    if (EFI_ERROR(st)) {
        return 0xFFFFFFFFu;
    }
    TpmRd r = { resp, resp_cap, 0, true };
    rd16(&r);                       /* tag */
    uint32_t rsize = rd32(&r);      /* responseSize */
    uint32_t rc = rd32(&r);         /* responseCode */
    if (!r.ok || rsize < 10 || rsize > resp_cap) {
        return 0xFFFFFFFFu;
    }
    r.len = rsize;
    *out_rd = r;
    return rc;
}

/* Write the standard empty-password authorization area (9 bytes). */
static void
wr_pw_auth(TpmWr *c)
{
    wr32(c, 9);              /* authorizationSize */
    wr32(c, TPM_RS_PW);      /* sessionHandle */
    wr16(c, 0);              /* nonce */
    wr8(c, 0);               /* sessionAttributes */
    wr16(c, 0);              /* hmac/password */
}

/* Write a single-bank (SHA-256) TPML_PCR_SELECTION for @p pcrs. */
static void
wr_pcr_selection(TpmWr *w, const uint32_t *pcrs, size_t n)
{
    wr32(w, 1);                 /* count: one TPMS_PCR_SELECTION */
    wr16(w, TPM_ALG_SHA256);    /* hash */
    wr8(w, 3);                  /* sizeofSelect: PCR 0..23 */
    uint8_t bm[3] = { 0, 0, 0 };
    for (size_t i = 0; i < n; i++) {
        bm[pcrs[i] >> 3] |= (uint8_t)(1u << (pcrs[i] & 7));
    }
    wr8(w, bm[0]);
    wr8(w, bm[1]);
    wr8(w, bm[2]);
}

/* Flush a transient handle (no-op for 0 or a permanent 0x40xxxxxx handle). */
static void
tpm_flush(EFI_TCG2_PROTOCOL *p, uint8_t *cmd, uint8_t *resp, size_t resp_cap,
          uint32_t handle)
{
    if (handle == 0 || (handle & 0xFF000000u) == 0x40000000u) {
        return;
    }
    TpmWr f = { cmd, 32, 0, true };
    wr16(&f, TPM_ST_NO_SESSIONS);
    wr32(&f, 14);
    wr32(&f, TPM_CC_FLUSH_CONTEXT);
    wr32(&f, handle);
    if (f.ok) {
        TpmRd r;
        tpm_submit(p, cmd, f.len, resp, resp_cap, &r);
    }
}

// ===================================================================
// Command builders
// ===================================================================

/* TPM2_CreatePrimary of the ECC SRK in the owner hierarchy. */
static bool
create_srk(EFI_TCG2_PROTOCOL *p, uint8_t *cmd, size_t cmd_cap,
           uint8_t *resp, size_t resp_cap, uint32_t *out_handle)
{
    uint8_t tpub[128];
    TpmWr t = { tpub, sizeof tpub, 0, true };
    wr16(&t, TPM_ALG_ECC);
    wr16(&t, TPM_ALG_SHA256);
    wr32(&t, SRK_ATTRIBUTES);
    wr16(&t, 0);                  /* authPolicy: empty */
    wr16(&t, TPM_ALG_AES);        /* symmetric: AES-128-CFB */
    wr16(&t, 128);
    wr16(&t, TPM_ALG_CFB);
    wr16(&t, TPM_ALG_NULL);       /* scheme */
    wr16(&t, TPM_ECC_NIST_P256);  /* curveID */
    wr16(&t, TPM_ALG_NULL);       /* kdf */
    wr16(&t, 0);                  /* unique X */
    wr16(&t, 0);                  /* unique Y */
    if (!t.ok) {
        return false;
    }

    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_CREATE_PRIMARY);
    wr32(&c, TPM_RH_OWNER);
    wr_pw_auth(&c);
    wr16(&c, 4); wr16(&c, 0); wr16(&c, 0);   /* inSensitive: empty */
    wr16(&c, (uint16_t)t.len); wr_bytes(&c, tpub, t.len);   /* inPublic */
    wr16(&c, 0);                  /* outsideInfo */
    wr32(&c, 0);                  /* creationPCR: empty */
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    if (tpm_submit(p, cmd, c.len, resp, resp_cap, &r) != 0) {
        return false;
    }
    *out_handle = rd32(&r);       /* objectHandle */
    return r.ok;
}

/* TPM2_PCR_Read the selection, hash the concatenated values -> @p digest[32]. */
static bool
pcr_policy_digest(EFI_TCG2_PROTOCOL *p, const uint32_t *pcrs, size_t n,
                  uint8_t *cmd, size_t cmd_cap, uint8_t *resp, size_t resp_cap,
                  uint8_t digest[32])
{
    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_NO_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_PCR_READ);
    wr_pcr_selection(&c, pcrs, n);
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    if (tpm_submit(p, cmd, c.len, resp, resp_cap, &r) != 0) {
        return false;
    }
    rd32(&r);                     /* pcrUpdateCounter */
    uint32_t sel = rd32(&r);      /* pcrSelectionOut.count */
    for (uint32_t i = 0; i < sel; i++) {
        rd16(&r);                 /* hash */
        rd_skip(&r, rd8(&r));     /* pcrSelect */
    }
    uint32_t dcount = rd32(&r);   /* pcrValues.count */
    uint8_t concat[32 * MAX_PCRS];
    size_t cl = 0;
    for (uint32_t i = 0; i < dcount; i++) {
        uint16_t ds = rd16(&r);
        for (uint16_t j = 0; j < ds; j++) {
            uint8_t b = rd8(&r);
            if (cl < sizeof concat) {
                concat[cl++] = b;
            }
        }
    }
    if (!r.ok || cl == 0) {
        return false;
    }

    /* PolicyPCR digest: H(zeros32 ‖ TPM_CC_PolicyPCR ‖ pcrs ‖ H(pcr values)). */
    uint8_t pcr_hash[32];
    if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, concat, cl,
                                    pcr_hash, 32) != AXL_OK) {
        return false;
    }
    uint8_t buf[128];
    TpmWr w = { buf, sizeof buf, 0, true };
    wr_zeros(&w, 32);
    wr32(&w, TPM_CC_POLICY_PCR);
    wr_pcr_selection(&w, pcrs, n);
    wr_bytes(&w, pcr_hash, 32);
    if (!w.ok) {
        return false;
    }
    return axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, buf, w.len,
                                       digest, 32) == AXL_OK;
}

/* Read a size-prefixed TPM2B and copy it verbatim (size + bytes) to @p out. */
static bool
copy_tpm2b(TpmRd *r, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t start = r->off;
    uint16_t sz = rd16(r);
    rd_skip(r, sz);
    if (!r->ok) {
        return false;
    }
    size_t total = (size_t)sz + 2;
    if (total > out_cap) {
        return false;
    }
    for (size_t i = 0; i < total; i++) {
        out[i] = r->p[start + i];
    }
    *out_len = total;
    return true;
}

/* TPM2_Create the keyedhash sealed object under @p srk. */
static bool
create_sealed(EFI_TCG2_PROTOCOL *p, uint32_t srk,
              const uint8_t *secret, size_t secret_len, const uint8_t policy[32],
              uint8_t *cmd, size_t cmd_cap, uint8_t *resp, size_t resp_cap,
              uint8_t *pub, size_t pub_cap, size_t *pub_len,
              uint8_t *priv, size_t priv_cap, size_t *priv_len)
{
    uint8_t kh[128];
    TpmWr t = { kh, sizeof kh, 0, true };
    wr16(&t, TPM_ALG_KEYEDHASH);
    wr16(&t, TPM_ALG_SHA256);
    wr32(&t, SEAL_ATTRIBUTES);
    wr16(&t, 32); wr_bytes(&t, policy, 32);   /* authPolicy */
    wr16(&t, TPM_ALG_NULL);                   /* keyedhash scheme */
    wr16(&t, 0);                              /* unique */
    if (!t.ok) {
        return false;
    }

    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_CREATE);
    wr32(&c, srk);
    wr_pw_auth(&c);
    /* inSensitive TPM2B_SENSITIVE_CREATE: userAuth(empty) + data(secret). */
    wr16(&c, (uint16_t)(4 + secret_len));
    wr16(&c, 0);
    wr16(&c, (uint16_t)secret_len); wr_bytes(&c, secret, secret_len);
    wr16(&c, (uint16_t)t.len); wr_bytes(&c, kh, t.len);   /* inPublic */
    wr16(&c, 0);                  /* outsideInfo */
    wr32(&c, 0);                  /* creationPCR */
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    if (tpm_submit(p, cmd, c.len, resp, resp_cap, &r) != 0) {
        return false;
    }
    rd32(&r);                     /* parameterSize */
    /* outPrivate then outPublic (each a size-prefixed TPM2B). */
    return copy_tpm2b(&r, priv, priv_cap, priv_len) &&
           copy_tpm2b(&r, pub, pub_cap, pub_len);
}

/* TPM2_Load the sealed object under @p srk. */
static bool
load_sealed(EFI_TCG2_PROTOCOL *p, uint32_t srk,
            const uint8_t *pub, size_t pub_len,
            const uint8_t *priv, size_t priv_len,
            uint8_t *cmd, size_t cmd_cap, uint8_t *resp, size_t resp_cap,
            uint32_t *out_handle)
{
    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_LOAD);
    wr32(&c, srk);
    wr_pw_auth(&c);
    wr_bytes(&c, priv, priv_len);   /* inPrivate (size-prefixed TPM2B) */
    wr_bytes(&c, pub, pub_len);     /* inPublic  (size-prefixed TPM2B) */
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    if (tpm_submit(p, cmd, c.len, resp, resp_cap, &r) != 0) {
        return false;
    }
    *out_handle = rd32(&r);
    return r.ok;
}

/* TPM2_StartAuthSession: an unbound, unsalted SHA-256 policy session. */
static bool
start_policy_session(EFI_TCG2_PROTOCOL *p, uint8_t *cmd, size_t cmd_cap,
                     uint8_t *resp, size_t resp_cap, uint32_t *out_session)
{
    uint8_t nonce[16];
    if (axl_rng_bytes(nonce, sizeof nonce) != AXL_OK) {
        axl_memset(nonce, 0xA5, sizeof nonce);   /* nonce value is not secret */
    }
    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_NO_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_START_AUTH_SESSION);
    wr32(&c, TPM_RH_NULL);        /* tpmKey */
    wr32(&c, TPM_RH_NULL);        /* bind */
    wr16(&c, 16); wr_bytes(&c, nonce, 16);    /* nonceCaller */
    wr16(&c, 0);                  /* encryptedSalt */
    wr8(&c, TPM_SE_POLICY);       /* sessionType */
    wr16(&c, TPM_ALG_NULL);       /* symmetric: none */
    wr16(&c, TPM_ALG_SHA256);     /* authHash */
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    if (tpm_submit(p, cmd, c.len, resp, resp_cap, &r) != 0) {
        return false;
    }
    *out_session = rd32(&r);
    return r.ok;
}

/* TPM2_PolicyPCR: bind the session to the live PCRs. */
static bool
policy_pcr(EFI_TCG2_PROTOCOL *p, uint32_t session, const uint32_t *pcrs, size_t n,
           uint8_t *cmd, size_t cmd_cap, uint8_t *resp, size_t resp_cap)
{
    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_NO_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_POLICY_PCR);
    wr32(&c, session);
    wr16(&c, 0);                  /* pcrDigest: empty -> use live PCRs */
    wr_pcr_selection(&c, pcrs, n);
    if (!c.ok) {
        return false;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    return tpm_submit(p, cmd, c.len, resp, resp_cap, &r) == 0;
}

/* TPM2_Unseal under the policy session. AXL_OK / AXL_DENIED (policy fail) /
   AXL_ERR. */
static int
unseal_obj(EFI_TCG2_PROTOCOL *p, uint32_t sealed, uint32_t session,
           uint8_t *cmd, size_t cmd_cap, uint8_t *resp, size_t resp_cap,
           uint8_t *out, size_t out_cap, size_t *out_len)
{
    TpmWr c = { cmd, cmd_cap, 0, true };
    wr16(&c, TPM_ST_SESSIONS);
    size_t sp = c.len;
    wr32(&c, 0);
    wr32(&c, TPM_CC_UNSEAL);
    wr32(&c, sealed);
    /* auth area: the policy session, continueSession set. */
    wr32(&c, 9);
    wr32(&c, session);
    wr16(&c, 0);                  /* nonceCaller */
    wr8(&c, 1);                   /* sessionAttributes: continueSession */
    wr16(&c, 0);                  /* hmac */
    if (!c.ok) {
        return AXL_ERR;
    }
    patch_size(cmd, sp, c.len);

    TpmRd r;
    uint32_t rc = tpm_submit(p, cmd, c.len, resp, resp_cap, &r);
    if (rc != 0) {
        return AXL_DENIED;        /* policy / authorization failure */
    }
    rd32(&r);                     /* parameterSize */
    uint16_t ds = rd16(&r);       /* outData size */
    if (!r.ok || ds > out_cap) {
        return AXL_ERR;
    }
    for (uint16_t i = 0; i < ds; i++) {
        out[i] = rd8(&r);
    }
    if (!r.ok) {
        return AXL_ERR;
    }
    *out_len = ds;
    return AXL_OK;
}

// ===================================================================
// Public API
// ===================================================================

int
axl_tpm_seal(
    const uint8_t  *secret,
    size_t          secret_len,
    const uint32_t *pcrs,
    size_t          pcr_count,
    uint8_t       **out_blob,
    size_t         *out_blob_len
    )
{
    if (secret == NULL || pcrs == NULL || out_blob == NULL ||
        out_blob_len == NULL) {
        return AXL_INVALID;
    }
    if (secret_len == 0 || secret_len > AXL_TPM_SEAL_MAX_SECRET ||
        pcr_count == 0 || pcr_count > MAX_PCRS) {
        return AXL_INVALID;
    }
    for (size_t i = 0; i < pcr_count; i++) {
        if (pcrs[i] > 23) {
            return AXL_INVALID;
        }
    }

    EFI_TCG2_PROTOCOL *p = seal_tcg2();
    if (p == NULL || p->SubmitCommand == NULL) {
        return AXL_ERR;
    }

    uint8_t cmd[1024];
    uint8_t resp[4096];
    uint32_t srk = 0;
    int rc = AXL_ERR;

    if (!create_srk(p, cmd, sizeof cmd, resp, sizeof resp, &srk)) {
        return AXL_ERR;
    }

    uint8_t policy[32];
    uint8_t pub[1024];
    uint8_t priv[1024];
    size_t pub_len = 0, priv_len = 0;
    if (pcr_policy_digest(p, pcrs, pcr_count, cmd, sizeof cmd, resp, sizeof resp,
                          policy) &&
        create_sealed(p, srk, secret, secret_len, policy,
                      cmd, sizeof cmd, resp, sizeof resp,
                      pub, sizeof pub, &pub_len, priv, sizeof priv, &priv_len)) {
        size_t blen = 6 + pcr_count + pub_len + priv_len;
        uint8_t *blob = axl_malloc(blen);
        if (blob != NULL) {
            TpmWr b = { blob, blen, 0, true };
            wr8(&b, 'A'); wr8(&b, 'T'); wr8(&b, 'S'); wr8(&b, '1');
            wr8(&b, 1);                       /* version */
            wr8(&b, (uint8_t)pcr_count);
            for (size_t i = 0; i < pcr_count; i++) {
                wr8(&b, (uint8_t)pcrs[i]);
            }
            wr_bytes(&b, pub, pub_len);
            wr_bytes(&b, priv, priv_len);
            if (b.ok) {
                *out_blob = blob;
                *out_blob_len = b.len;
                rc = AXL_OK;
            } else {
                axl_free(blob);
            }
        }
    }

    tpm_flush(p, cmd, resp, sizeof resp, srk);
    /* cmd carried the plaintext secret inside the Create command. */
    axl_memset(cmd, 0, sizeof cmd);
    axl_memset(resp, 0, sizeof resp);
    return rc;
}

int
axl_tpm_unseal(
    const uint8_t *blob,
    size_t         blob_len,
    uint8_t      **out_secret,
    size_t        *out_secret_len
    )
{
    if (blob == NULL || out_secret == NULL || out_secret_len == NULL) {
        return AXL_INVALID;
    }

    TpmRd b = { blob, blob_len, 0, true };
    if (rd8(&b) != 'A' || rd8(&b) != 'T' || rd8(&b) != 'S' || rd8(&b) != '1' ||
        rd8(&b) != 1) {
        return AXL_INVALID;
    }
    uint8_t pcr_count = rd8(&b);
    if (pcr_count == 0 || pcr_count > MAX_PCRS) {
        return AXL_INVALID;
    }
    uint32_t pcrs[MAX_PCRS];
    for (uint8_t i = 0; i < pcr_count; i++) {
        uint32_t v = rd8(&b);
        if (v > 23) {        /* untrusted blob: keep the bitmap index in range */
            return AXL_INVALID;
        }
        pcrs[i] = v;
    }
    size_t pub_off = b.off;
    uint16_t pub_sz = rd16(&b);
    rd_skip(&b, pub_sz);
    size_t priv_off = b.off;
    uint16_t priv_sz = rd16(&b);
    rd_skip(&b, priv_sz);
    if (!b.ok) {
        return AXL_INVALID;
    }
    const uint8_t *pub = blob + pub_off;
    size_t pub_len = (size_t)pub_sz + 2;
    const uint8_t *priv = blob + priv_off;
    size_t priv_len = (size_t)priv_sz + 2;

    EFI_TCG2_PROTOCOL *p = seal_tcg2();
    if (p == NULL || p->SubmitCommand == NULL) {
        return AXL_ERR;
    }

    uint8_t cmd[1024];
    uint8_t resp[4096];
    uint32_t srk = 0, sealed = 0, session = 0;
    int rc = AXL_ERR;

    if (create_srk(p, cmd, sizeof cmd, resp, sizeof resp, &srk) &&
        load_sealed(p, srk, pub, pub_len, priv, priv_len,
                    cmd, sizeof cmd, resp, sizeof resp, &sealed) &&
        start_policy_session(p, cmd, sizeof cmd, resp, sizeof resp, &session) &&
        policy_pcr(p, session, pcrs, pcr_count, cmd, sizeof cmd, resp,
                   sizeof resp)) {
        uint8_t secret[AXL_TPM_SEAL_MAX_SECRET];
        size_t secret_len = 0;
        int urc = unseal_obj(p, sealed, session, cmd, sizeof cmd, resp,
                             sizeof resp, secret, sizeof secret, &secret_len);
        if (urc == AXL_OK) {
            uint8_t *out = axl_malloc(secret_len);
            if (out != NULL) {
                axl_memcpy(out, secret, secret_len);
                *out_secret = out;
                *out_secret_len = secret_len;
                rc = AXL_OK;
            }
            axl_memset(secret, 0, sizeof secret);
        } else {
            rc = urc;             /* AXL_DENIED on policy fail */
        }
    }

    tpm_flush(p, cmd, resp, sizeof resp, session);
    tpm_flush(p, cmd, resp, sizeof resp, sealed);
    tpm_flush(p, cmd, resp, sizeof resp, srk);
    /* resp carried the recovered secret in the Unseal response. */
    axl_memset(cmd, 0, sizeof cmd);
    axl_memset(resp, 0, sizeof resp);
    return rc;
}
