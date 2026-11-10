/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-ssh-host-key.c -- the PROVIDER-FREE half of <axl/axl-ssh-host-key.h>:
 * the fingerprint and public line, the openssh-key-v1 parser, the file
 * reader, and the default variable's read / write / delete.
 *
 * NOTHING HERE MAY REACH THE Ed25519 PROVIDER. sshd's launcher links this
 * file to fingerprint the key its resident driver serves, and the provider
 * would cost it ~120 KB it does not have. SHA-256, base64 and the SSH wire
 * helpers only; AxlTestSshHostKey asserts axl_pk_alg_available(ED25519) is
 * false in an image that calls every function here. The provider-bound half
 * is axl-ssh-host-key-load.c.
 *
 * Every buffer that held private bytes is zeroed with axl_ssh_secure_zero
 * before it is freed, on every path: the file, its transcoding, the base64
 * text, the decoded blob, and every variable payload. Each is sized once
 * from its input -- nothing grows by realloc, which would leave copies.
 */

#include <axl/axl-ssh-host-key.h>
#include <axl/axl-ssh-core.h>
#include <axl/axl-digest.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-var.h>

#include "axl-ssh-internal.h"
#include "axl-ssh-host-key-internal.h"
#include "../fs/axl-fs-internal.h"
#include "../util/axl-var-internal.h"

AXL_LOG_DOMAIN("ssh");

/* openssh-key-v1 (OpenSSH PROTOCOL.key; sshkey.c:72-77). */
#define MARK_BEGIN       "-----BEGIN OPENSSH PRIVATE KEY-----"
#define MARK_END         "-----END OPENSSH PRIVATE KEY-----"
#define AUTH_MAGIC       "openssh-key-v1"            /* and its NUL */
#define AUTH_MAGIC_LEN   15u
#define NONE_BLOCKSIZE   8u   /* cipher "none"'s block size (cipher.c) */
#define ED25519_SK_LEN   64u  /* seed || public (ssh-ed25519.c:132) */

/* The stored variable: a private format, versioned so the in-memory record
   can change without stored keys becoming unreadable (contract review M8).
   "AXK", version, then the PKCS#8 and the public key. */
#define STORED_MAGIC_LEN  3u
#define STORED_VERSION    1u
#define STORED_LEN        (STORED_MAGIC_LEN + 1u + AXL_SSH_HOST_KEY_PKCS8_LEN \
                           + AXL_SSH_ED25519_PUBLIC_LEN)

/* The attributes as the firmware reports them. AXL_NV_* deliberately mirror
   the EFI_VARIABLE_* bit positions (axl-var.h), so they compare with the raw
   word. 0x10 is EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS, deprecated and
   named by no AXL flag. */
#define ATTRS_WANTED      (AXL_NV_PERSISTENT | AXL_NV_BOOT)
#define ATTRS_AUTH_ANY    (0x10u | AXL_VAR_TIME_AUTH_WRITE | AXL_VAR_ENHANCED_AUTH)

/* A refused key type, by the name it carries, and the line naming it. */
typedef struct {
    const char *type;
    const char *why;
} RefusedType;

#define REFUSED(t)  { t, t " host keys are not supported; only ssh-ed25519" }

static const RefusedType REFUSED_TYPES[] = {
    REFUSED("ssh-rsa"),
    REFUSED("ssh-dss"),
    REFUSED("ecdsa-sha2-nistp256"),
    REFUSED("ecdsa-sha2-nistp384"),
    REFUSED("ecdsa-sha2-nistp521"),
    REFUSED("sk-ssh-ed25519@openssh.com"),
    REFUSED("sk-ecdsa-sha2-nistp256@openssh.com"),
    REFUSED("ssh-ed25519-cert-v01@openssh.com"),
    REFUSED("ssh-rsa-cert-v01@openssh.com"),
    REFUSED("ssh-dss-cert-v01@openssh.com"),
    REFUSED("ecdsa-sha2-nistp256-cert-v01@openssh.com"),
    REFUSED("ecdsa-sha2-nistp384-cert-v01@openssh.com"),
    REFUSED("ecdsa-sha2-nistp521-cert-v01@openssh.com"),
    REFUSED("sk-ssh-ed25519-cert-v01@openssh.com"),
    REFUSED("sk-ecdsa-sha2-nistp256-cert-v01@openssh.com"),
};

/* PEM headers of formats that are not openssh-key-v1, each refused by what
   it is. Matched as a prefix of the first line. */
typedef struct {
    const char *header;
    int         rc;
    const char *why;
} RefusedPem;

static const RefusedPem REFUSED_PEMS[] = {
    { "-----BEGIN PRIVATE KEY-----", AXL_UNSUPPORTED, AXL_SSH_HK_WHY_PKCS8 },
    { "-----BEGIN ENCRYPTED PRIVATE KEY-----", AXL_UNSUPPORTED,
      AXL_SSH_HK_WHY_PASSPHRASE },
    { "-----BEGIN RSA PRIVATE KEY-----", AXL_UNSUPPORTED,
      "ssh-rsa host keys are not supported; only ssh-ed25519" },
    { "-----BEGIN DSA PRIVATE KEY-----", AXL_UNSUPPORTED,
      "ssh-dss host keys are not supported; only ssh-ed25519" },
    { "-----BEGIN EC PRIVATE KEY-----", AXL_UNSUPPORTED,
      "ecdsa host keys are not supported; only ssh-ed25519" },
};

static const AxlGuid HOST_KEY_GUID = AXL_SSH_HOST_KEY_GUID;

const uint8_t axl_ssh_hk_pkcs8_prefix[AXL_SSH_HK_PKCS8_PREFIX_LEN] = {
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
    0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20
};

/* Production's answer at every test point: never. A unit image links a
   strong definition (see axl-ssh-host-key-internal.h). */
__attribute__((weak)) bool
_axl_ssh_host_key_test_point(const char *point)
{
    (void)point;
    return false;
}

static void
set_why(const char **why, const char *reason)
{
    if (why != NULL) {
        *why = reason;
    }
}

/* Zero @a n bytes at @a p, then free it. NULL-safe. */
static void
free_secure(void *p, size_t n)
{
    if (p != NULL) {
        axl_ssh_secure_zero(p, n);
        axl_free(p);
    }
}

bool
axl_ssh_hk_pkcs8_is_ed25519(const uint8_t *pkcs8)
{
    return axl_memcmp(pkcs8, axl_ssh_hk_pkcs8_prefix,
                      sizeof axl_ssh_hk_pkcs8_prefix) == 0;
}

void
axl_ssh_host_key_record_clear(AxlSshHostKeyRecord *rec)
{
    if (rec != NULL) {
        axl_ssh_secure_zero(rec, sizeof *rec);
    }
}

// ---------------------------------------------------------------------------
// Fingerprint and public line
// ---------------------------------------------------------------------------

/* The base64 of the `ssh-ed25519` key blob, or of its SHA-256 when
   @a hashed, with the padding dropped when @a unpadded. axl_free() it. */
static int
blob_base64(const uint8_t *pub, bool hashed, bool unpadded, char **out)
{
    AxlString *blob = axl_string_new("");
    uint8_t    digest[32];
    char      *b64  = NULL;

    *out = NULL;
    if (blob == NULL || axl_ssh_hostkey_blob(blob, pub) != AXL_OK) {
        axl_string_free(blob);
        return AXL_NO_RESOURCES;
    }
    if (hashed) {
        if (axl_compute_checksum_digest(AXL_CHECKSUM_SHA256, axl_string_data(blob),
                                        axl_string_len(blob), digest,
                                        sizeof digest) == AXL_OK) {
            b64 = axl_base64_encode(digest, sizeof digest);
        }
    } else {
        b64 = axl_base64_encode(axl_string_data(blob), axl_string_len(blob));
    }
    axl_string_free(blob);
    if (b64 == NULL) {
        return AXL_NO_RESOURCES;
    }
    if (unpadded) {
        size_t n = axl_strlen(b64);
        while (n > 0 && b64[n - 1] == '=') {
            b64[--n] = '\0';
        }
    }
    *out = b64;
    return AXL_OK;
}

static int
format_key(const uint8_t *pub, size_t len, char *out, size_t cap, size_t need,
           bool fingerprint)
{
    if (pub == NULL || out == NULL || len != AXL_SSH_ED25519_PUBLIC_LEN
        || cap < need) {
        return AXL_INVALID;
    }
    char *b64 = NULL;
    int   rc  = blob_base64(pub, fingerprint, fingerprint, &b64);
    if (rc != AXL_OK) {
        return rc;
    }
    /* Exactly @a need, or an error -- never a truncation. The base64 has a
       fixed length here, and a snprintf that cut a wrong one to fit would
       hand back a plausible string: an unstripped `=` lands exactly where
       the buffer ends. */
    const char *prefix = fingerprint ? "SHA256:" : AXL_SSH_HOSTKEY_NAME " ";
    if (axl_strlen(prefix) + axl_strlen(b64) + 1 != need) {
        axl_free(b64);
        return AXL_ERR;
    }
    axl_snprintf(out, cap, "%s%s", prefix, b64);
    axl_free(b64);
    return AXL_OK;
}

int
axl_ssh_host_key_fingerprint(const uint8_t *public_key, size_t len, char *out,
                             size_t cap)
{
    return format_key(public_key, len, out, cap, AXL_SSH_FINGERPRINT_SIZE, true);
}

int
axl_ssh_host_key_public_line(const uint8_t *public_key, size_t len, char *out,
                             size_t cap)
{
    return format_key(public_key, len, out, cap, AXL_SSH_PUBLIC_LINE_SIZE, false);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

static bool
str_is(const uint8_t *p, uint32_t n, const char *lit)
{
    size_t l = axl_strlen(lit);
    return n == l && axl_memcmp(p, lit, l) == 0;
}

/* The line at @a text[*pos], without its LF or CRLF; *pos moves past it.
   False at the end of the text. */
static bool
next_line(const char *text, size_t tlen, size_t *pos, const char **line,
          size_t *line_len)
{
    if (*pos >= tlen) {
        return false;
    }
    size_t start = *pos;
    size_t end   = start;
    while (end < tlen && text[end] != '\n') {
        end++;
    }
    *pos      = end < tlen ? end + 1 : end;
    *line     = text + start;
    *line_len = end - start;
    if (*line_len > 0 && (*line)[*line_len - 1] == '\r') {
        (*line_len)--;
    }
    return true;
}

static bool
line_is(const char *line, size_t len, const char *lit)
{
    return len == axl_strlen(lit) && axl_memcmp(line, lit, len) == 0;
}

/* The envelope's key type, refused unless ssh-ed25519. AXL_OK to go on. */
static int
check_type(const uint8_t *type, uint32_t type_len, const char **reason)
{
    if (str_is(type, type_len, AXL_SSH_HOSTKEY_NAME)) {
        return AXL_OK;
    }
    for (size_t i = 0; i < AXL_ARRAY_SIZE(REFUSED_TYPES); i++) {
        if (str_is(type, type_len, REFUSED_TYPES[i].type)) {
            *reason = REFUSED_TYPES[i].why;
            return AXL_UNSUPPORTED;
        }
    }
    *reason = "unknown host key type; only ssh-ed25519 is supported";
    return AXL_UNSUPPORTED;
}

/* The decoded openssh-key-v1 blob, checked as sshkey.c's private2_decrypt
   and sshkey_parse_private2 check it, plus two stricter checks. In place:
   the caller zeroes @a b. */
static int
parse_blob(const uint8_t *b, size_t blen, AxlSshHostKeyRecord *out,
           const char **reason)
{
    const uint8_t *cipher, *kdf, *kdfopts, *pub, *type;
    uint32_t       cipher_len, kdf_len, kdfopts_len, pub_len, type_len;
    uint32_t       nkeys = 0, enc_len = 0;
    size_t         off = AUTH_MAGIC_LEN;
    uint8_t        env_pub[AXL_SSH_ED25519_PUBLIC_LEN];

    *reason = AXL_SSH_HK_WHY_MALFORMED;
    if (blen < AUTH_MAGIC_LEN || axl_memcmp(b, AUTH_MAGIC, AUTH_MAGIC_LEN) != 0
        || axl_ssh_get_string(b, blen, &off, &cipher, &cipher_len) != AXL_OK
        || axl_ssh_get_string(b, blen, &off, &kdf, &kdf_len) != AXL_OK
        || axl_ssh_get_string(b, blen, &off, &kdfopts, &kdfopts_len) != AXL_OK
        || axl_ssh_get_u32(b, blen, &off, &nkeys) != AXL_OK || nkeys == 0) {
        return AXL_INVALID;
    }
    if (nkeys != 1) {
        *reason = "more than one key in the file";
        return AXL_INVALID;
    }
    if (axl_ssh_get_string(b, blen, &off, &pub, &pub_len) != AXL_OK) {
        return AXL_INVALID;
    }
    /* The TYPE first, before the cipher, as sshkey_froms reads it first
       (sshkey.c:3095): an encrypted RSA key is refused as RSA. */
    size_t toff = 0;
    if (axl_ssh_get_string(pub, pub_len, &toff, &type, &type_len) != AXL_OK) {
        return AXL_INVALID;
    }
    int rc = check_type(type, type_len, reason);
    if (rc != AXL_OK) {
        return rc;
    }
    /* Whole blob consumed, 32-byte key (sshkey.c:2055, ssh-ed25519.c:112). */
    if (axl_ssh_hostkey_blob_parse(pub, pub_len, env_pub) != AXL_OK
        || axl_ssh_get_u32(b, blen, &off, &enc_len) != AXL_OK) {
        *reason = AXL_SSH_HK_WHY_MALFORMED;
        return AXL_INVALID;
    }
    if (str_is(kdf, kdf_len, "bcrypt")) {
        *reason = AXL_SSH_HK_WHY_PASSPHRASE;
        return AXL_UNSUPPORTED;
    }
    /* Stricter than sshkey.c: KDF options must be empty for KDF none. */
    if (!str_is(cipher, cipher_len, "none") || !str_is(kdf, kdf_len, "none")
        || kdfopts_len != 0 || enc_len < NONE_BLOCKSIZE
        || enc_len % NONE_BLOCKSIZE != 0 || blen - off < enc_len) {
        *reason = AXL_SSH_HK_WHY_MALFORMED;
        return AXL_INVALID;
    }
    if (blen - off > enc_len) {
        *reason = "trailing data after the key";
        return AXL_INVALID;
    }

    /* The private section. */
    const uint8_t *p = b + off;
    size_t         poff = 0;
    uint32_t       check1 = 0, check2 = 0;
    const uint8_t *ptype, *pk, *sk, *comment;
    uint32_t       ptype_len, pk_len, sk_len, comment_len;

    if (axl_ssh_get_u32(p, enc_len, &poff, &check1) != AXL_OK
        || axl_ssh_get_u32(p, enc_len, &poff, &check2) != AXL_OK) {
        return AXL_INVALID;
    }
    if (check1 != check2) {
        *reason = "corrupt key (check values differ)";
        return AXL_INVALID;
    }
    if (axl_ssh_get_string(p, enc_len, &poff, &ptype, &ptype_len) != AXL_OK) {
        return AXL_INVALID;
    }
    if (!str_is(ptype, ptype_len, AXL_SSH_HOSTKEY_NAME)) {
        *reason = AXL_SSH_HK_WHY_HALVES;
        return AXL_INVALID;
    }
    if (axl_ssh_get_string(p, enc_len, &poff, &pk, &pk_len) != AXL_OK
        || pk_len != AXL_SSH_ED25519_PUBLIC_LEN
        || axl_ssh_get_string(p, enc_len, &poff, &sk, &sk_len) != AXL_OK
        || sk_len != ED25519_SK_LEN
        || axl_ssh_get_string(p, enc_len, &poff, &comment, &comment_len) != AXL_OK
        /* sshbuf_get_cstring refuses a comment with a NUL in it. */
        || axl_memchr(comment, 0, comment_len) != NULL) {
        *reason = AXL_SSH_HK_WHY_MALFORMED;
        return AXL_INVALID;
    }
    /* The 1, 2, 3... padding (sshkey.c private2_check_padding). */
    for (size_t i = 1; poff < enc_len; i++, poff++) {
        if (p[poff] != (uint8_t)(i & 0xffu)) {
            *reason = "corrupt key (bad padding)";
            return AXL_INVALID;
        }
    }
    /* The envelope's key is the private section's (sshkey.c:3239), and --
       stricter -- so is the 64-byte secret's public half. */
    if (axl_memcmp(env_pub, pk, AXL_SSH_ED25519_PUBLIC_LEN) != 0
        || axl_memcmp(sk + AXL_SSH_HK_SEED_LEN, pk, AXL_SSH_ED25519_PUBLIC_LEN) != 0) {
        *reason = AXL_SSH_HK_WHY_HALVES;
        return AXL_INVALID;
    }
    axl_memcpy(out->pkcs8, axl_ssh_hk_pkcs8_prefix, sizeof axl_ssh_hk_pkcs8_prefix);
    axl_memcpy(out->pkcs8 + sizeof axl_ssh_hk_pkcs8_prefix, sk, AXL_SSH_HK_SEED_LEN);
    axl_memcpy(out->public_key, pk, AXL_SSH_ED25519_PUBLIC_LEN);
    *reason = NULL;
    return AXL_OK;
}

/* The UTF-8 text of a file: its armour, then the blob. */
static int
parse_text(const char *text, size_t tlen, AxlSshHostKeyRecord *out,
           const char **reason)
{
    size_t      pos = 0;
    const char *line;
    size_t      line_len;

    *reason = AXL_SSH_HK_WHY_NOT_KEY;
    if (!next_line(text, tlen, &pos, &line, &line_len)) {
        return AXL_INVALID;
    }
    if (!line_is(line, line_len, MARK_BEGIN)) {
        for (size_t i = 0; i < AXL_ARRAY_SIZE(REFUSED_PEMS); i++) {
            if (line_is(line, line_len, REFUSED_PEMS[i].header)) {
                *reason = REFUSED_PEMS[i].why;
                return REFUSED_PEMS[i].rc;
            }
        }
        return AXL_INVALID;
    }

    /* The base64 between the armour lines, without line ends. Sized once
       from the text, which bounds it. */
    char *b64 = axl_malloc(tlen + 1);
    if (b64 == NULL) {
        *reason = NULL;
        return AXL_NO_RESOURCES;
    }
    size_t n     = 0;
    bool   ended = false;
    while (next_line(text, tlen, &pos, &line, &line_len)) {
        if (line_is(line, line_len, MARK_END)) {
            ended = true;
            break;
        }
        /* Spaces and tabs inside a line are skipped, as OpenSSH's b64_pton
           skips them: an editor that leaves trailing blanks makes a file
           OpenSSH still reads. */
        for (size_t k = 0; k < line_len; k++) {
            if (line[k] != ' ' && line[k] != '\t') {
                b64[n++] = line[k];
            }
        }
    }
    b64[n] = '\0';

    int      rc   = AXL_INVALID;
    void    *blob = NULL;
    size_t   blen = 0;
    *reason = AXL_SSH_HK_WHY_MALFORMED;
    if (ended && n > 0 && axl_base64_decode(b64, &blob, &blen) == AXL_OK) {
        rc = parse_blob(blob, blen, out, reason);
    }
    free_secure(blob, blen);
    free_secure(b64, tlen + 1);
    return rc;
}

int
axl_ssh_host_key_parse(const void *data, size_t len, AxlSshHostKeyRecord *out,
                       const char **why)
{
    const char *reason = AXL_SSH_HK_WHY_NOT_KEY;

    if (out != NULL) {
        axl_ssh_host_key_record_clear(out);
    }
    if (data == NULL || out == NULL) {
        set_why(why, reason);
        return AXL_INVALID;
    }
    if (len > AXL_SSH_HOST_KEY_FILE_MAX) {
        set_why(why, AXL_SSH_HK_WHY_TOO_LARGE);
        return AXL_INVALID;
    }
    char  *text     = NULL;
    size_t text_len = 0;
    int    rc       = axl_text_to_utf8(data, len, &text, &text_len, NULL);
    if (rc == AXL_OK) {
        rc = parse_text(text, text_len, out, &reason);
    } else if (rc != AXL_NO_RESOURCES) {
        rc = AXL_INVALID;   /* UTF-16 that does not convert */
    }
    free_secure(text, text_len + 1);
    if (rc != AXL_OK) {
        axl_ssh_host_key_record_clear(out);
        set_why(why, rc == AXL_NO_RESOURCES ? "out of memory" : reason);
    }
    return rc;
}

int
axl_ssh_host_key_read_file(const char *path, AxlSshHostKeyRecord *out,
                           const char **why)
{
    AxlFsEntry e;

    if (out != NULL) {
        axl_ssh_host_key_record_clear(out);
    }
    if (path == NULL || out == NULL) {
        set_why(why, "no such file");
        return AXL_INVALID;
    }
    if (axl_file_info(path, &e) != AXL_OK || axl_fs_entry_is_dir(&e)) {
        set_why(why, "no such file");
        return AXL_NOT_FOUND;
    }
    /* By its SIZE, before a byte is read: a 2 GB file on the ESP is refused,
       not allocated. */
    if (e.size > AXL_SSH_HOST_KEY_FILE_MAX) {
        set_why(why, AXL_SSH_HK_WHY_TOO_LARGE);
        return AXL_INVALID;
    }
    /* The one file reader both sshd images already link (final review F5):
       it zeroes what a short read returned before freeing it, so partial
       private-key text never stays in the pool -- the path an unreliable
       virtual-media read takes. A file that changed size since the check
       above is read at its new size, and the parse refuses one past
       AXL_SSH_HOST_KEY_FILE_MAX. */
    void     *buf = NULL;
    size_t    got = 0;
    AxlStatus st  = axl_file_get_contents_status(path, true, &buf, &got);
    if (st == AXL_NO_RESOURCES) {
        set_why(why, "out of memory");
        return AXL_NO_RESOURCES;
    }
    if (st == AXL_NOT_FOUND) {
        /* Removed since the size check: missing, as above (re-review N3). */
        set_why(why, "no such file");
        return AXL_NOT_FOUND;
    }
    if (st != AXL_OK) {
        set_why(why, "cannot read the file");
        return AXL_IO_ERROR;
    }
    int rc = axl_ssh_host_key_parse(buf, got, out, why);
    free_secure(buf, got + 1);
    return rc;
}

// ---------------------------------------------------------------------------
// The default variable
// ---------------------------------------------------------------------------

/* What is wrong with a stored value, or NULL. On NULL @a out holds it. */
const char *
axl_ssh_hk_stored_check(uint32_t attrs, const uint8_t *data, size_t size,
             AxlSshHostKeyRecord *out)
{
    /* Attributes first, before a byte of the value is used: a variable the
       OS could have planted is refused whatever it holds. */
    if ((attrs & ATTRS_AUTH_ANY) != 0) {
        return "the stored key is an authenticated variable; only its signer"
               " can delete it";
    }
    if ((attrs & AXL_NV_RUNTIME) != 0) {
        return "the stored key is readable by the OS (runtime access)";
    }
    if (attrs != ATTRS_WANTED) {
        return "the stored key has the wrong attributes (not NV+BS)";
    }
    if (size != STORED_LEN) {
        return "the stored key has the wrong size";
    }
    if (axl_memcmp(data, "AXK", STORED_MAGIC_LEN) != 0) {
        return "the stored key is not in AxlSsh's format";
    }
    if (data[STORED_MAGIC_LEN] != STORED_VERSION) {
        return "the stored key's format version is not supported";
    }
    const uint8_t *pkcs8 = data + STORED_MAGIC_LEN + 1;
    if (!axl_ssh_hk_pkcs8_is_ed25519(pkcs8)) {
        return "the stored key is not an Ed25519 key";
    }
    axl_memcpy(out->pkcs8, pkcs8, AXL_SSH_HOST_KEY_PKCS8_LEN);
    axl_memcpy(out->public_key, pkcs8 + AXL_SSH_HOST_KEY_PKCS8_LEN,
               AXL_SSH_ED25519_PUBLIC_LEN);
    return NULL;
}

int
axl_ssh_host_key_nv_read(AxlSshHostKeyRecord *out, const char **why)
{
    if (out == NULL) {
        return AXL_INVALID;
    }
    axl_ssh_host_key_record_clear(out);
    if (_axl_ssh_host_key_test_point("nv-read")) {
        set_why(why, AXL_SSH_HK_WHY_READ);
        return AXL_IO_ERROR;
    }
    uint32_t attrs = 0;
    void    *data  = NULL;
    size_t   size  = 0;
    int      rc    = axl_var_read_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, &attrs,
                                      &data, &size);
    if (rc == AXL_NOT_FOUND) {
        set_why(why, "no host key is stored");
        return AXL_NOT_FOUND;
    }
    if (rc == AXL_NO_RESOURCES) {
        set_why(why, "out of memory");
        return AXL_NO_RESOURCES;
    }
    if (rc != AXL_OK) {
        /* NOT "absent": a read that failed says nothing about whether a key
           is there, and a caller that generated one here would overwrite
           it. */
        set_why(why, AXL_SSH_HK_WHY_READ);
        return AXL_IO_ERROR;
    }
    const char *reason = axl_ssh_hk_stored_check(attrs, data, size, out);
    free_secure(data, size);
    if (reason != NULL) {
        axl_ssh_host_key_record_clear(out);
        set_why(why, reason);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_ssh_host_key_nv_write(const AxlSshHostKeyRecord *rec, const char **why)
{
    if (rec == NULL) {
        return AXL_INVALID;
    }
    if (!axl_ssh_hk_pkcs8_is_ed25519(rec->pkcs8)) {
        set_why(why, "the key is not an Ed25519 key");
        return AXL_INVALID;
    }
    size_t size = 0;
    int    rc   = axl_var_read_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, NULL, NULL,
                                   &size);
    if (rc == AXL_OK) {
        set_why(why, "a host key is already stored");
        return AXL_EXISTS;
    }
    if (rc != AXL_NOT_FOUND) {
        set_why(why, AXL_SSH_HK_WHY_READ);
        return AXL_IO_ERROR;
    }

    uint8_t blob[STORED_LEN];
    axl_memcpy(blob, "AXK", STORED_MAGIC_LEN);
    blob[STORED_MAGIC_LEN] = STORED_VERSION;
    axl_memcpy(blob + STORED_MAGIC_LEN + 1, rec->pkcs8, AXL_SSH_HOST_KEY_PKCS8_LEN);
    axl_memcpy(blob + STORED_MAGIC_LEN + 1 + AXL_SSH_HOST_KEY_PKCS8_LEN,
               rec->public_key, AXL_SSH_ED25519_PUBLIC_LEN);

    rc = _axl_ssh_host_key_test_point("nv-write")
         ? AXL_ERR
         : axl_var_write_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, ATTRS_WANTED,
                             blob, sizeof blob);
    if (rc != AXL_OK) {
        axl_ssh_secure_zero(blob, sizeof blob);
        set_why(why, "cannot write the variable");
        return AXL_IO_ERROR;
    }

    /* Read back: a write the firmware accepted and did not keep -- a full
       store that reports success, a flash that does not hold the bits --
       would otherwise be found only at the next start, as a changed key. */
    uint32_t attrs = 0;
    void    *data  = NULL;
    size     = 0;
    rc       = axl_var_read_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, &attrs, &data,
                                &size);
    bool same = rc == AXL_OK && attrs == ATTRS_WANTED && size == sizeof blob
                && axl_memcmp(data, blob, sizeof blob) == 0
                && !_axl_ssh_host_key_test_point("nv-readback");
    free_secure(data, size);
    axl_ssh_secure_zero(blob, sizeof blob);
    if (!same) {
        /* Best effort: what did land must not load next start as a key no
           client ever saw. */
        if (_axl_ssh_host_key_test_point("nv-cleanup-refused")
            || axl_var_write_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, 0, NULL, 0)
                != AXL_OK) {
            axl_debug("host key: could not delete a write that did not read back");
        }
        set_why(why, "the variable did not read back");
        return AXL_IO_ERROR;
    }
    return AXL_OK;
}

int
axl_ssh_host_key_nv_delete(void)
{
    int rc = _axl_ssh_host_key_test_point("nv-delete-denied")
             ? AXL_DENIED
             : axl_var_write_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, 0, NULL, 0);
    if (rc == AXL_OK || rc == AXL_NOT_FOUND) {
        return AXL_OK;
    }
    if (rc == AXL_DENIED) {
        return AXL_DENIED;
    }
    /* EDK2 answers an unsigned delete of a time-authenticated variable with
       EFI_INVALID_PARAMETER, not a security violation: the variable's own
       attributes say which refusal this was. */
    uint32_t attrs = 0;
    size_t   size  = 0;
    if (axl_var_read_raw(AXL_SSH_HOST_KEY_VAR, &HOST_KEY_GUID, &attrs, NULL, &size)
            == AXL_OK
        && (attrs & ATTRS_AUTH_ANY) != 0) {
        return AXL_DENIED;
    }
    return AXL_IO_ERROR;
}
