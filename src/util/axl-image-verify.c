/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-image-verify.c
    Standalone PE Authenticode signature inspection.

    Two-axis check (presence + db validity); see axl-image-verify.h
    for the per-field contract. The presence check is a pure file-
    bytes parse of the PE Certificate Table data directory. The
    db-validation check leans on the firmware's PE loader: we ask
    LoadImage to take our bytes via SourceBuffer; under Secure Boot
    that runs the signature check before signaling success, and
    returns EFI_SECURITY_VIOLATION on mismatch (per UEFI 2.10
    §7.4.1). We immediately UnloadImage either way — the image is
    never started.

    Field offsets here come straight from the PE/COFF spec rather
    than EDK2's `IndustryStandard/PeImage.h` because that header
    isn't part of the generated UEFI surface and pulling it in just
    for two struct walks would multiply the include footprint.
**/

#include <axl/axl-image-verify.h>
#include <axl/axl-fs.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include "../backend/axl-backend.h"

AXL_LOG_DOMAIN("image-verify");

/* PE Optional Header magic constants (PE/COFF §3.4). */
#define PE_OPT_MAGIC_PE32      0x010Bu
#define PE_OPT_MAGIC_PE32_PLUS 0x020Bu

/* DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] index (PE/COFF §3.4.6 +
   §6.4). SECURITY is index 4 — Certificate Table. */
#define PE_DATA_DIR_SECURITY   4u

/* Minimum sizes for the various headers, used as bounds checks
   before reading fields. */
#define DOS_HEADER_MIN         0x40u  /* up through e_lfanew at 0x3C */
#define NT_SIG_SIZE            4u     /* "PE\0\0" */
#define FILE_HEADER_SIZE       20u
#define OPT_HEADER_MIN_PE32    96u    /* fields up through DataDirectory count */
#define OPT_HEADER_MIN_PE32P   112u

static inline uint16_t
read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t
read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Locate the Certificate Table data directory in @p buf, of size
   @p size. On success populates *out_cert_offset / *out_cert_size
   with the file-offset and byte count of the cert blob; on
   failure returns -1.

   The cert table is one of the few PE data directories whose
   VirtualAddress field is a FILE OFFSET, not an RVA, per PE/COFF
   §6.4 — convenient for us since we never have to walk section
   tables to translate. */
static int
locate_certificate_table(
    const uint8_t  *buf,
    size_t          size,
    uint32_t       *out_cert_offset,
    uint32_t       *out_cert_size
    )
{
    if (size < DOS_HEADER_MIN) {
        return -1;
    }
    /* DOS header: e_magic at 0x00 == 'MZ', e_lfanew at 0x3C. */
    if (buf[0] != 'M' || buf[1] != 'Z') {
        return -1;
    }
    /* All offset arithmetic widens to uint64 to defeat a hostile
       e_lfanew near 0xFFFFFFFF wrapping the uint32 sum back into a
       small value. Real PE files top out well below 4 GiB so the
       widen is just defense in depth, but free either way. */
    uint64_t pe_offset = read_u32_le(&buf[0x3C]);
    if (pe_offset + NT_SIG_SIZE + FILE_HEADER_SIZE > (uint64_t)size) {
        return -1;
    }
    /* NT signature: "PE\0\0". */
    if (buf[pe_offset]     != 'P'
        || buf[pe_offset + 1] != 'E'
        || buf[pe_offset + 2] != 0
        || buf[pe_offset + 3] != 0)
    {
        return -1;
    }
    uint64_t opt_offset = pe_offset + NT_SIG_SIZE + FILE_HEADER_SIZE;
    if (opt_offset + 2 > (uint64_t)size) {
        return -1;
    }
    uint16_t opt_magic = read_u16_le(&buf[opt_offset]);
    /* DataDirectory[] starts at different offsets in PE32 vs PE32+. */
    uint64_t data_dir_offset;
    if (opt_magic == PE_OPT_MAGIC_PE32) {
        if (opt_offset + OPT_HEADER_MIN_PE32 > (uint64_t)size) {
            return -1;
        }
        data_dir_offset = opt_offset + OPT_HEADER_MIN_PE32;
    } else if (opt_magic == PE_OPT_MAGIC_PE32_PLUS) {
        if (opt_offset + OPT_HEADER_MIN_PE32P > (uint64_t)size) {
            return -1;
        }
        data_dir_offset = opt_offset + OPT_HEADER_MIN_PE32P;
    } else {
        return -1;
    }
    /* DataDirectory[SECURITY] is entry 4; each entry is 8 bytes
       (uint32 VirtualAddress + uint32 Size). */
    uint64_t cert_dir_offset = data_dir_offset + (PE_DATA_DIR_SECURITY * 8u);
    if (cert_dir_offset + 8u > (uint64_t)size) {
        return -1;
    }
    *out_cert_offset = read_u32_le(&buf[cert_dir_offset]);
    *out_cert_size   = read_u32_le(&buf[cert_dir_offset + 4]);
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal DER walker + Authenticode → X.509 Subject/Issuer CN extraction
//
// What this is: a hand-rolled, intentionally small ASN.1 DER reader good
// enough to walk a PKCS#7 SignedData blob, locate the first X.509
// certificate in its certificates SET, and pull the CommonName attribute
// out of the cert's Issuer and Subject Name fields. Everything we need
// for diagnostic output of the signing certificate.
//
// What this is NOT: a general-purpose ASN.1 library. We don't validate
// constructed/primitive bits, multi-byte tags, indefinite-length
// encodings (which DER forbids anyway), or anything beyond the encodings
// Authenticode actually emits. Strings outside PrintableString / UTF8String
// are reported as NULL CN — the consumer's `do trust` displays "(unknown)"
// rather than risking a malformed-string crash. All bounds checks widen
// to size_t to defeat pointer-overflow on hostile inputs.
// ---------------------------------------------------------------------------

/* Tag numbers used below. Single-byte tags only (high tag-number form
   doesn't appear in PKCS#7 / X.509 at the levels we walk). */
#define DER_TAG_INTEGER          0x02
#define DER_TAG_OCTET_STRING     0x04
#define DER_TAG_OID              0x06
#define DER_TAG_UTF8_STRING      0x0C
#define DER_TAG_PRINTABLE_STRING 0x13
#define DER_TAG_SEQUENCE         0x30
#define DER_TAG_SET              0x31
#define DER_TAG_CONTEXT_0        0xA0   /* [0] EXPLICIT or IMPLICIT */
#define DER_TAG_CONTEXT_3        0xA3

/* CommonName OID = 2.5.4.3 → DER: 06 03 55 04 03. We compare the
   value bytes only; the surrounding 06 03 prefix is the tag/length of
   an OID that the walker has already consumed. */
static const uint8_t OID_CN_VALUE[] = { 0x55, 0x04, 0x03 };

/* signedData OID = 1.2.840.113549.1.7.2 → 09-byte value. */
static const uint8_t OID_SIGNED_DATA_VALUE[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02
};

/* Decode one DER TLV. Sets *out_tag to the tag byte, *out_value to the
   start of the value, *out_len to the value length, and advances *cur
   past the TLV. Bounds-checked end-to-end. Returns 0 on success, -1
   when the tag/length doesn't fit in the remaining bytes. */
static int
der_read_tlv(const uint8_t **cur, const uint8_t *end,
             uint8_t *out_tag, const uint8_t **out_value, size_t *out_len)
{
    const uint8_t *p = *cur;
    if (p >= end) {
        return -1;
    }
    uint8_t tag = *p++;
    if (p >= end) {
        return -1;
    }
    uint8_t  l0 = *p++;
    size_t   len;
    if ((l0 & 0x80u) == 0) {
        /* Short form: length fits in one byte. */
        len = l0;
    } else {
        size_t nbytes = l0 & 0x7Fu;
        /* DER caps length-of-length sensibly; 4 bytes covers any
           realistic Authenticode field. Refuse anything wider. */
        if (nbytes == 0 || nbytes > 4 || (size_t)(end - p) < nbytes) {
            return -1;
        }
        len = 0;
        for (size_t i = 0; i < nbytes; i++) {
            len = (len << 8) | (size_t)*p++;
        }
    }
    if ((size_t)(end - p) < len) {
        return -1;
    }
    *out_tag   = tag;
    *out_value = p;
    *out_len   = len;
    *cur       = p + len;
    return 0;
}

/* Variant that also requires the tag to match @p expected. Returns 0
   on success, -1 on tag mismatch or bounds error. */
static int
der_expect(const uint8_t **cur, const uint8_t *end, uint8_t expected,
           const uint8_t **out_value, size_t *out_len)
{
    const uint8_t *save = *cur;
    uint8_t tag;
    if (der_read_tlv(cur, end, &tag, out_value, out_len) != 0) {
        return -1;
    }
    if (tag != expected) {
        *cur = save;
        return -1;
    }
    return 0;
}

/* Skip one TLV without inspecting it. */
static int
der_skip(const uint8_t **cur, const uint8_t *end)
{
    uint8_t        tag;
    const uint8_t *val;
    size_t         len;
    return der_read_tlv(cur, end, &tag, &val, &len);
}

/* Allocate a NUL-terminated UTF-8 copy of a PrintableString or
   UTF8String value. Returns NULL on alloc failure or unsupported
   encoding. PrintableString is a strict ASCII subset so we copy
   bytes through; UTF8String needs no transcoding. */
static char *
der_string_to_utf8(uint8_t tag, const uint8_t *val, size_t len)
{
    if (tag != DER_TAG_PRINTABLE_STRING && tag != DER_TAG_UTF8_STRING) {
        /* T61String / BMPString / IA5String / etc. — not supported.
           Caller renders "(unknown)" in diagnostic output rather
           than risking a malformed-string crash. */
        return NULL;
    }
    char *s = axl_malloc(len + 1);
    if (s == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        s[i] = (char)val[i];
    }
    s[len] = '\0';
    return s;
}

/* Walk a Name (RDNSequence) and return the first CommonName as a
   heap-allocated UTF-8 string, or NULL on parse failure / no CN
   found / unsupported string encoding.

   Intentionally NOT static so the unit-test harness can invoke it
   against hand-crafted Name fixtures without needing a full PE +
   PKCS#7 stack. NOT part of the public surface — no header decl
   ships in include/axl/. The leading `_axl_` prefix matches the
   project convention for non-public symbols. */
char *
_axl_image_verify_name_extract_cn(const uint8_t *name_seq_value,
                                  size_t          name_seq_len)
{
    /* Sidestep the C11 NULL-pointer-arithmetic pedantry: defined
       in C2X but implementation-defined in C11. Real-world impls
       all behave fine, but a top-of-function guard is clearer
       than relying on the loop's `cur < end` to short-circuit. */
    if (name_seq_value == NULL || name_seq_len == 0) {
        return NULL;
    }
    /* Name ::= SEQUENCE OF RelativeDistinguishedName
       RelativeDistinguishedName ::= SET OF AttributeTypeAndValue
       AttributeTypeAndValue ::= SEQUENCE { type OID, value ANY }
       The caller already unwrapped the outer SEQUENCE; we receive
       the value bytes that contain a stream of RDN SETs. */
    const uint8_t *cur = name_seq_value;
    const uint8_t *end = name_seq_value + name_seq_len;

    while (cur < end) {
        const uint8_t *rdn_val;
        size_t         rdn_len;
        if (der_expect(&cur, end, DER_TAG_SET, &rdn_val, &rdn_len) != 0) {
            return NULL;
        }
        const uint8_t *attr_cur = rdn_val;
        const uint8_t *attr_end = rdn_val + rdn_len;
        while (attr_cur < attr_end) {
            const uint8_t *atv_val;
            size_t         atv_len;
            if (der_expect(&attr_cur, attr_end, DER_TAG_SEQUENCE,
                           &atv_val, &atv_len) != 0) {
                return NULL;
            }
            const uint8_t *t_cur = atv_val;
            const uint8_t *t_end = atv_val + atv_len;
            const uint8_t *oid_val;
            size_t         oid_len;
            if (der_expect(&t_cur, t_end, DER_TAG_OID,
                           &oid_val, &oid_len) != 0) {
                return NULL;
            }
            uint8_t        str_tag;
            const uint8_t *str_val;
            size_t         str_len;
            if (der_read_tlv(&t_cur, t_end,
                             &str_tag, &str_val, &str_len) != 0) {
                return NULL;
            }
            if (oid_len == sizeof(OID_CN_VALUE)
                && axl_memcmp(oid_val, OID_CN_VALUE, sizeof(OID_CN_VALUE)) == 0)
            {
                /* Found CN — first match wins per RFC 5280 §4.1.2.4
                   (a Name should have exactly one CN attribute). */
                return der_string_to_utf8(str_tag, str_val, str_len);
            }
        }
    }
    return NULL;
}

/* Walk a PKCS#7 SignedData blob, find the first X.509 certificate's
   TBSCertificate, and extract Subject + Issuer CommonNames into the
   caller's struct. Best-effort: any parse failure leaves the fields
   NULL without mutating the caller's success flags. */
static void
extract_signing_cert_cns(
    const uint8_t          *blob,
    size_t                  blob_len,
    AxlImageSignatureInfo  *info
    )
{
    const uint8_t *cur = blob;
    const uint8_t *end = blob + blob_len;
    const uint8_t *seq_val;
    size_t         seq_len;

    /* ContentInfo ::= SEQUENCE { contentType OID, content [0] EXPLICIT ANY } */
    if (der_expect(&cur, end, DER_TAG_SEQUENCE, &seq_val, &seq_len) != 0) {
        return;
    }
    cur = seq_val;
    end = seq_val + seq_len;

    /* contentType OID — must be signedData (1.2.840.113549.1.7.2). */
    const uint8_t *oid_val;
    size_t         oid_len;
    if (der_expect(&cur, end, DER_TAG_OID, &oid_val, &oid_len) != 0) {
        return;
    }
    if (oid_len != sizeof(OID_SIGNED_DATA_VALUE)
        || axl_memcmp(oid_val, OID_SIGNED_DATA_VALUE,
                      sizeof(OID_SIGNED_DATA_VALUE)) != 0)
    {
        return;
    }

    /* [0] EXPLICIT content → SignedData SEQUENCE. */
    if (der_expect(&cur, end, DER_TAG_CONTEXT_0, &seq_val, &seq_len) != 0) {
        return;
    }
    cur = seq_val;
    end = seq_val + seq_len;
    if (der_expect(&cur, end, DER_TAG_SEQUENCE, &seq_val, &seq_len) != 0) {
        return;
    }
    cur = seq_val;
    end = seq_val + seq_len;

    /* SignedData fields: version INTEGER, digestAlgorithms SET,
       contentInfo SEQUENCE, then [0] IMPLICIT certificates SET. */
    if (der_skip(&cur, end) != 0) return;   /* version */
    if (der_skip(&cur, end) != 0) return;   /* digestAlgorithms */
    if (der_skip(&cur, end) != 0) return;   /* contentInfo */

    const uint8_t *certs_val;
    size_t         certs_len;
    if (der_expect(&cur, end, DER_TAG_CONTEXT_0,
                   &certs_val, &certs_len) != 0)
    {
        /* No certificates present (rare; signed data without bundled
           certs would need an external chain). Nothing to extract. */
        return;
    }

    /* Take the FIRST certificate. For Authenticode-signed PEs the
       signer cert is conventionally first; the formal way is to
       match against SignerInfo's IssuerAndSerial, but that
       complexity isn't justified for diagnostic-only CN output. */
    const uint8_t *cert_cur = certs_val;
    const uint8_t *cert_end = certs_val + certs_len;
    const uint8_t *cert_val;
    size_t         cert_len;
    if (der_expect(&cert_cur, cert_end, DER_TAG_SEQUENCE,
                   &cert_val, &cert_len) != 0)
    {
        return;
    }

    /* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm,
                                  signatureValue }. We only need TBS. */
    cur = cert_val;
    end = cert_val + cert_len;
    const uint8_t *tbs_val;
    size_t         tbs_len;
    if (der_expect(&cur, end, DER_TAG_SEQUENCE, &tbs_val, &tbs_len) != 0) {
        return;
    }

    /* TBSCertificate fields:
       - [0] EXPLICIT version (optional; absent → v1)
       - serialNumber INTEGER
       - signature AlgorithmIdentifier SEQUENCE
       - issuer Name (SEQUENCE)
       - validity SEQUENCE
       - subject Name (SEQUENCE)
       - ... rest unused
       Detect optional version by tag-peeking. */
    cur = tbs_val;
    end = tbs_val + tbs_len;
    if (cur < end && *cur == DER_TAG_CONTEXT_0) {
        if (der_skip(&cur, end) != 0) return;
    }
    if (der_skip(&cur, end) != 0) return;   /* serialNumber */
    if (der_skip(&cur, end) != 0) return;   /* signature */

    const uint8_t *issuer_val;
    size_t         issuer_len;
    if (der_expect(&cur, end, DER_TAG_SEQUENCE,
                   &issuer_val, &issuer_len) != 0)
    {
        return;
    }
    char *iss_cn = _axl_image_verify_name_extract_cn(issuer_val, issuer_len);

    if (der_skip(&cur, end) != 0) {                 /* validity */
        if (iss_cn != NULL) axl_free(iss_cn);
        return;
    }

    const uint8_t *subject_val;
    size_t         subject_len;
    if (der_expect(&cur, end, DER_TAG_SEQUENCE,
                   &subject_val, &subject_len) != 0)
    {
        if (iss_cn != NULL) axl_free(iss_cn);
        return;
    }
    char *subj_cn = _axl_image_verify_name_extract_cn(subject_val, subject_len);

    /* Best-effort: we set whichever fields we found. If only one
       CN parses, the other stays NULL. The caller's success flags
       (has_signature, signature_valid) are NOT mutated by this
       function — CN extraction is purely informational. */
    info->subject_cn = subj_cn;
    info->issuer_cn  = iss_cn;
}

/* Locate the WIN_CERTIFICATE blob in the PE file, validate its type,
   and call the CN extractor on the embedded PKCS#7 SignedData. */
static void
extract_cns_from_cert_table(
    const uint8_t          *file_buf,
    size_t                  file_size,
    uint32_t                cert_offset,
    uint32_t                cert_size,
    AxlImageSignatureInfo  *info
    )
{
    /* WIN_CERTIFICATE header is 8 bytes:
         dwLength         (4 bytes, includes header)
         wRevision        (2 bytes; 0x0200 for v2)
         wCertificateType (2 bytes; 0x0002 = WIN_CERT_TYPE_PKCS_SIGNED_DATA)
       The cert table can hold multiple WIN_CERTIFICATE entries
       padded to 8 bytes; for Authenticode we only care about the
       first one. */
    if (cert_size < 8) {
        return;
    }
    if ((uint64_t)cert_offset + cert_size > (uint64_t)file_size) {
        return;
    }
    const uint8_t *cert_buf = file_buf + cert_offset;
    uint32_t       win_len  = read_u32_le(&cert_buf[0]);
    uint16_t       win_type = read_u16_le(&cert_buf[6]);
    if (win_len < 8 || win_len > cert_size) {
        return;
    }
    if (win_type != 0x0002 /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */) {
        /* Other types (X.509-only, EFI-GUID-wrapped) aren't covered
           by the diagnostic CN extractor in this revision. */
        return;
    }
    extract_signing_cert_cns(cert_buf + 8, win_len - 8, info);
}

/* Try to ask the firmware to dry-run a signature check.
   Returns:
     0  → consulted, valid signature
     1  → consulted, INVALID signature
    -1  → not consulted (Secure Boot off, LoadImage refused
          for non-security reason, etc.). */
static int
loadimage_dry_run_validate(
    const uint8_t  *buf,
    size_t          size
    )
{
    if (axl_bs() == NULL || axl_bs()->LoadImage == NULL) {
        return -1;
    }
    EFI_HANDLE  child = NULL;
    EFI_STATUS  status = axl_bs()->LoadImage(
        FALSE,                                 /* BootPolicy */
        gImageHandle,                          /* ParentImageHandle */
        NULL,                                  /* FilePath */
        (VOID *)(uintptr_t)buf,                /* SourceBuffer */
        (UINTN)size,                           /* SourceSize */
        &child);
    /* Per UEFI 2.10 §7.4.1, EFI_SECURITY_VIOLATION still returns a
       valid image handle that MUST be unloaded. EFI_SUCCESS also
       returns a handle. Other failure codes generally don't, but
       the unload is still NULL-safe so we always try. */
    int rc;
    if (status == EFI_SECURITY_VIOLATION) {
        rc = 1;
    } else if (status == EFI_SUCCESS) {
        rc = 0;
    } else {
        /* Includes EFI_NOT_FOUND ("no Security2 protocol — Secure
           Boot off or otherwise unenforced"), EFI_LOAD_ERROR,
           EFI_INVALID_PARAMETER, etc. We can't tell "Secure Boot
           is off" from "PE is malformed" reliably here, so report
           "not consulted" and let the caller's presence-only
           result stand. */
        axl_debug("LoadImage dry-run returned 0x%llx; db not consulted",
                  (unsigned long long)status);
        rc = -1;
    }
    if (child != NULL && axl_bs()->UnloadImage != NULL) {
        (void)axl_bs()->UnloadImage(child);
    }
    return rc;
}

int
axl_image_verify_signature(
    const char             *path,
    bool                    consult_db,
    AxlImageSignatureInfo  *info
    )
{
    /* Clear info FIRST so every -1 path that follows leaves it in
       the documented "unknown / nothing detected" state rather than
       arbitrary leftover bytes. The NULL-info case is the only -1
       return the caller can't observe through the struct anyway. */
    if (info == NULL) {
        return AXL_ERR;
    }
    info->has_signature   = false;
    info->signature_valid = false;
    info->consulted_db    = false;
    info->subject_cn      = NULL;
    info->issuer_cn       = NULL;

    if (path == NULL) {
        return AXL_ERR;
    }

    void   *buf = NULL;
    size_t  size = 0;
    if (axl_file_get_contents(path, &buf, &size) != AXL_OK || buf == NULL) {
        return AXL_ERR;
    }

    uint32_t cert_offset = 0;
    uint32_t cert_size   = 0;
    int parse_rc = locate_certificate_table((const uint8_t *)buf, size,
                                            &cert_offset, &cert_size);
    if (parse_rc != 0) {
        /* Not a recognizable PE image — caller can't make any
           statement about its signature. */
        axl_free(buf);
        return AXL_ERR;
    }

    info->has_signature = (cert_size > 0);

    if (info->has_signature) {
        extract_cns_from_cert_table((const uint8_t *)buf, size,
                                    cert_offset, cert_size, info);
    }

    if (consult_db) {
        int dry = loadimage_dry_run_validate((const uint8_t *)buf, size);
        if (dry == 0) {
            info->signature_valid = true;
            info->consulted_db    = true;
        } else if (dry == 1) {
            info->signature_valid = false;
            info->consulted_db    = true;
        } else {
            /* Not consulted — fall through to presence-only signal. */
            info->signature_valid = info->has_signature;
            info->consulted_db    = false;
        }
    } else {
        /* Caller didn't ask for db validation; presence-only. */
        info->signature_valid = info->has_signature;
    }

    axl_free(buf);
    return AXL_OK;
}

void
axl_image_signature_info_free(
    AxlImageSignatureInfo  *info
    )
{
    if (info == NULL) {
        return;
    }
    /* subject_cn / issuer_cn are heap-allocated UTF-8 strings
       extracted from the signing certificate (when has_signature
       is true and the cert parses); free each independently and
       NULL the pointers so a defensive double-free is a no-op. */
    if (info->subject_cn != NULL) {
        axl_free(info->subject_cn);
        info->subject_cn = NULL;
    }
    if (info->issuer_cn != NULL) {
        axl_free(info->issuer_cn);
        info->issuer_cn = NULL;
    }
}
