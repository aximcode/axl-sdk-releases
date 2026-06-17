/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tpm.c
    TPM 2.0 presence and capability readout.

    Wraps the firmware's TCG2 protocol (a singleton, located lazily and
    cached like the CPU-arch / MP-services helpers). `axl_tpm_present`
    reports whether the protocol is published; `axl_tpm_get_capability`
    calls GetCapability and projects the boot-service capability into a
    typed AxlTpmCapability. No measurement, event-log, or PCR-extension
    surface is exposed.
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>   /* EFI_TCG2_PROTOCOL (extra) */
#include <axl/axl-str.h>     /* axl_memset */
#include <axl/axl-tpm.h>

/* EFI_TCG2_PROTOCOL_GUID — defined in the TCG EFI Protocol spec, not in
   generated/guids.h (same value as the "tcg2" well-known entry in
   axl-protocol.c). */
static const EFI_GUID TCG2_GUID = {
    0x607f766c, 0x7455, 0x42be,
    { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f }
};

/* Lazy-locate + cache the singleton, like cpu_arch() / mp_services(). */
static EFI_TCG2_PROTOCOL *g_tcg2;
static bool               g_probed;

static EFI_TCG2_PROTOCOL *
tcg2(void)
{
    if (g_probed) {
        return g_tcg2;
    }
    g_probed = true;
    EFI_STATUS status = axl_efi_call(
        axl_bs()->LocateProtocol, 3,
        (EFI_GUID *)&TCG2_GUID, NULL, (void **)&g_tcg2);
    if (EFI_ERROR(status)) {
        g_tcg2 = NULL;
    }
    return g_tcg2;
}

bool
axl_tpm_present(void)
{
    return tcg2() != NULL;
}

int
axl_tpm_get_capability(
    AxlTpmCapability *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    EFI_TCG2_PROTOCOL *p = tcg2();
    if (p == NULL || p->GetCapability == NULL) {
        return AXL_ERR;
    }

    /* GetCapability is an IN OUT call: the caller sets Size to the
       buffer size so the firmware knows how much it may fill (the
       struct-ver >= 1.1 fields are only written when Size is large
       enough). Zero first so unfilled tail fields read 0. */
    EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
    axl_memset(&cap, 0, sizeof(cap));
    cap.Size = (UINT8)sizeof(cap);

    EFI_STATUS status = axl_efi_call(p->GetCapability, 2, p, &cap);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    out->present                 = cap.TPMPresentFlag != 0;
    out->structure_version_major = cap.StructureVersion.Major;
    out->structure_version_minor = cap.StructureVersion.Minor;
    out->protocol_version_major  = cap.ProtocolVersion.Major;
    out->protocol_version_minor  = cap.ProtocolVersion.Minor;
    out->manufacturer_id         = cap.ManufacturerID;
    out->max_command_size        = cap.MaxCommandSize;
    out->max_response_size       = cap.MaxResponseSize;
    out->number_of_pcr_banks       = cap.NumberOfPcrBanks;
    out->supported_hash_algorithms = cap.HashAlgorithmBitmap;
    out->active_pcr_banks          = cap.ActivePcrBanks;
    return AXL_OK;
}

// ===================================================================
// Endorsement Key public read (raw TPM2 over SubmitCommand)
// ===================================================================
//
// TPM structures are big-endian on the wire. We build a TPM2_Create
// Primary in the endorsement hierarchy with the standard TCG EK template
// (ECC P-256 first, RSA-2048 fallback), read the public area out of the
// response, extract its `unique` field (ECC point X||Y, or RSA modulus),
// and flush the transient primary handle. Deterministic per TPM.

/* TPM2 constants (TCG TPM 2.0 Structures / EK Credential Profile). */
#define TPM_ST_SESSIONS        0x8002u
#define TPM_ST_NO_SESSIONS     0x8001u
#define TPM_CC_CREATE_PRIMARY  0x00000131u
#define TPM_CC_FLUSH_CONTEXT   0x00000165u
#define TPM_RH_ENDORSEMENT     0x4000000Bu
#define TPM_RS_PW              0x40000009u
#define TPM_ALG_RSA            0x0001u
#define TPM_ALG_ECC            0x0023u
#define TPM_ALG_SHA256         0x000Bu
#define TPM_ALG_AES            0x0006u
#define TPM_ALG_CFB            0x0043u
#define TPM_ALG_NULL           0x0010u
#define TPM_ECC_NIST_P256      0x0003u
#define EK_OBJECT_ATTRIBUTES   0x000300B2u  /* fixedTPM|fixedParent|sensitive
                                               DataOrigin|adminWithPolicy|
                                               restricted|decrypt */

/* The well-known SHA-256 EK authorization policy (TCG EK Credential
   Profile, the PolicySecret(TPM_RH_ENDORSEMENT) digest). */
static const uint8_t EK_AUTH_POLICY[32] = {
    0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xb3, 0xf8,
    0x1a, 0x90, 0xcc, 0x8d, 0x46, 0xa5, 0xd7, 0x24,
    0xfd, 0x52, 0xd7, 0x6e, 0x06, 0x52, 0x0b, 0x64,
    0xf2, 0xa1, 0xda, 0x1b, 0x33, 0x14, 0x69, 0xaa
};

// ---- big-endian write cursor ----
typedef struct {
    uint8_t *p;
    size_t   cap;
    size_t   len;
    bool     ok;
} TpmWr;

static void
wr8(TpmWr *w, uint8_t v)
{
    if (w->len < w->cap) {
        w->p[w->len++] = v;
    } else {
        w->ok = false;
    }
}
static void wr16(TpmWr *w, uint16_t v) { wr8(w, (uint8_t)(v >> 8)); wr8(w, (uint8_t)v); }
static void wr32(TpmWr *w, uint32_t v) { wr16(w, (uint16_t)(v >> 16)); wr16(w, (uint16_t)v); }
static void
wr_zeros(TpmWr *w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        wr8(w, 0);
    }
}

// ---- big-endian read cursor (bounds-checked) ----
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         off;
    bool           ok;
} TpmRd;

static uint8_t
rd8(TpmRd *r)
{
    if (r->off < r->len) {
        return r->p[r->off++];
    }
    r->ok = false;
    return 0;
}
static uint16_t rd16(TpmRd *r) { uint16_t h = rd8(r); return (uint16_t)((h << 8) | rd8(r)); }
static uint32_t rd32(TpmRd *r) { uint32_t h = rd16(r); return (h << 16) | rd16(r); }
static void
rd_skip(TpmRd *r, size_t n)
{
    if (r->off + n <= r->len) {
        r->off += n;
    } else {
        r->ok  = false;
        r->off = r->len;
    }
}

/* Build the TCG EK template TPMT_PUBLIC for @p alg into @p w. */
static void
ek_template(TpmWr *w, AxlTpmEkAlg alg)
{
    wr16(w, alg == AXL_TPM_EK_ECC_P256 ? TPM_ALG_ECC : TPM_ALG_RSA);  /* type */
    wr16(w, TPM_ALG_SHA256);                                          /* nameAlg */
    wr32(w, EK_OBJECT_ATTRIBUTES);                                    /* objectAttributes */
    wr16(w, 32);                                                      /* authPolicy size */
    for (size_t i = 0; i < 32; i++) {
        wr8(w, EK_AUTH_POLICY[i]);
    }
    /* parameters: symmetric AES-128-CFB, scheme NULL (both templates) */
    wr16(w, TPM_ALG_AES);
    wr16(w, 128);
    wr16(w, TPM_ALG_CFB);
    wr16(w, TPM_ALG_NULL);                                            /* scheme */
    if (alg == AXL_TPM_EK_ECC_P256) {
        wr16(w, TPM_ECC_NIST_P256);                                  /* curveID */
        wr16(w, TPM_ALG_NULL);                                       /* kdf */
        /* unique TPMS_ECC_POINT: zero-filled X and Y (32 each) */
        wr16(w, 32); wr_zeros(w, 32);
        wr16(w, 32); wr_zeros(w, 32);
    } else {
        wr16(w, 2048);                                               /* keyBits */
        wr32(w, 0);                                                  /* exponent (default) */
        /* unique TPM2B_PUBLIC_KEY_RSA: zero-filled 256-byte modulus */
        wr16(w, 256); wr_zeros(w, 256);
    }
}

/* Parse the response's outPublic and copy the `unique` bytes (ECC X||Y
   or RSA modulus) into @p out. Assumes the EK template shape (AES
   symmetric, NULL schemes). Returns the byte count, or 0 on failure. */
static size_t
ek_extract_unique(TpmRd *r, AxlTpmEkAlg alg, uint8_t *out, size_t out_cap)
{
    rd16(r);                 /* outPublic TPM2B size (outer) */
    uint16_t type = rd16(r); /* TPMT_PUBLIC.type */
    rd16(r);                 /* nameAlg */
    rd32(r);                 /* objectAttributes */
    rd_skip(r, rd16(r));     /* authPolicy: size + bytes */
    /* symmetric TPMT_SYM_DEF_OBJECT */
    if (rd16(r) != TPM_ALG_NULL) {
        rd16(r);             /* keyBits */
        rd16(r);             /* mode */
    }
    rd16(r);                 /* scheme (NULL for EK) */

    size_t n = 0;
    if (type == TPM_ALG_ECC) {
        rd16(r);             /* curveID */
        rd16(r);             /* kdf scheme (NULL) */
        uint16_t xs = rd16(r);
        for (uint16_t i = 0; i < xs; i++) { uint8_t b = rd8(r); if (n < out_cap) out[n] = b; n++; }
        uint16_t ys = rd16(r);
        for (uint16_t i = 0; i < ys; i++) { uint8_t b = rd8(r); if (n < out_cap) out[n] = b; n++; }
    } else {
        rd16(r);             /* keyBits */
        rd32(r);             /* exponent */
        uint16_t ms = rd16(r);
        for (uint16_t i = 0; i < ms; i++) { uint8_t b = rd8(r); if (n < out_cap) out[n] = b; n++; }
    }

    if (!r->ok || n == 0 || n > out_cap) {
        return 0;
    }
    return n;
}

/* Derive the EK of @p alg and copy its public `unique` bytes into @p out
   (capacity @p out_cap). Returns the byte count, or 0 on any failure. */
static size_t
ek_derive(EFI_TCG2_PROTOCOL *p, AxlTpmEkAlg alg, uint8_t *out, size_t out_cap)
{
    uint8_t cmd[1024];
    uint8_t resp[2048];

    /* TPMT_PUBLIC template (built first so we can length-prefix it). */
    uint8_t tpub[512];
    TpmWr   t = { tpub, sizeof(tpub), 0, true };
    ek_template(&t, alg);
    if (!t.ok) {
        return 0;
    }

    TpmWr c = { cmd, sizeof(cmd), 0, true };
    wr16(&c, TPM_ST_SESSIONS);
    size_t size_pos = c.len;
    wr32(&c, 0);                       /* commandSize (patched below) */
    wr32(&c, TPM_CC_CREATE_PRIMARY);
    wr32(&c, TPM_RH_ENDORSEMENT);      /* primaryHandle */
    /* authorization area: a single empty password session */
    wr32(&c, 9);                       /* authorizationSize */
    wr32(&c, TPM_RS_PW);
    wr16(&c, 0);                       /* nonce */
    wr8(&c, 0);                        /* sessionAttributes */
    wr16(&c, 0);                       /* hmac/password */
    /* inSensitive TPM2B_SENSITIVE_CREATE: empty userAuth + data */
    wr16(&c, 4);
    wr16(&c, 0);
    wr16(&c, 0);
    /* inPublic TPM2B_PUBLIC */
    wr16(&c, (uint16_t)t.len);
    for (size_t i = 0; i < t.len; i++) {
        wr8(&c, tpub[i]);
    }
    wr16(&c, 0);                       /* outsideInfo */
    wr32(&c, 0);                       /* creationPCR: empty selection */
    if (!c.ok) {
        return 0;
    }
    /* patch commandSize */
    uint32_t total = (uint32_t)c.len;
    cmd[size_pos + 0] = (uint8_t)(total >> 24);
    cmd[size_pos + 1] = (uint8_t)(total >> 16);
    cmd[size_pos + 2] = (uint8_t)(total >> 8);
    cmd[size_pos + 3] = (uint8_t)total;

    axl_memset(resp, 0, sizeof(resp));
    EFI_STATUS st = axl_efi_call(p->SubmitCommand, 5, p,
                                 (UINT32)c.len, cmd,
                                 (UINT32)sizeof(resp), resp);
    if (EFI_ERROR(st)) {
        return 0;
    }

    /* Parse response header. */
    TpmRd r = { resp, sizeof(resp), 0, true };
    rd16(&r);                          /* tag */
    uint32_t rsize = rd32(&r);         /* responseSize */
    uint32_t rc    = rd32(&r);         /* responseCode (0 == success) */
    /* Reject a non-success or malformed (out-of-range size) response
       rather than parsing against the rest of the buffer. */
    if (!r.ok || rc != 0 || rsize < 10 || rsize > sizeof(resp)) {
        return 0;
    }
    r.len = rsize;
    uint32_t handle = rd32(&r);        /* objectHandle (transient) */
    rd32(&r);                          /* parameterSize (tag == SESSIONS) */

    size_t n = ek_extract_unique(&r, alg, out, out_cap);

    /* Flush the transient primary regardless of parse outcome. */
    TpmWr f = { cmd, sizeof(cmd), 0, true };
    wr16(&f, TPM_ST_NO_SESSIONS);
    wr32(&f, 14);                      /* fixed command size */
    wr32(&f, TPM_CC_FLUSH_CONTEXT);
    wr32(&f, handle);
    if (f.ok) {
        axl_efi_call(p->SubmitCommand, 5, p,
                     (UINT32)f.len, cmd, (UINT32)sizeof(resp), resp);
    }

    return n;
}

int
axl_tpm_read_ek_pub(
    uint8_t     *buf,
    size_t       buf_size,
    size_t      *out_len,
    AxlTpmEkAlg *out_alg
    )
{
    if (out_len == NULL) {
        return AXL_ERR;
    }
    EFI_TCG2_PROTOCOL *p = tcg2();
    if (p == NULL || p->SubmitCommand == NULL) {
        return AXL_ERR;
    }

    /* ECC P-256 first (smaller, faster), then RSA-2048. */
    static const AxlTpmEkAlg order[] = { AXL_TPM_EK_ECC_P256, AXL_TPM_EK_RSA2048 };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint8_t pub[256];
        size_t  n = ek_derive(p, order[i], pub, sizeof(pub));
        if (n == 0) {
            continue;
        }
        if (out_alg != NULL) {
            *out_alg = order[i];
        }
        if (buf == NULL) {            /* size query */
            *out_len = n;
            return AXL_OK;
        }
        if (buf_size < n) {           /* too small */
            *out_len = n;
            return AXL_ERR;
        }
        axl_memcpy(buf, pub, n);
        *out_len = n;
        return AXL_OK;
    }
    return AXL_ERR;
}

bool
axl_tpm_ek_available(void)
{
    static int cached;   /* 0 unknown, 1 yes, 2 no */
    if (cached == 0) {
        size_t n = 0;
        cached = (tcg2() != NULL
                  && axl_tpm_read_ek_pub(NULL, 0, &n, NULL) == AXL_OK)
                 ? 1 : 2;
    }
    return cached == 1;
}
