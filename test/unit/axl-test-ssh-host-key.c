/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-ssh-host-key.c
    Unit tests for the PROVIDER-FREE half of <axl/axl-ssh-host-key.h>:
    the fingerprint and public line, the openssh-key-v1 parser, the file
    reader, and the default variable's read / write / delete.

    A SEPARATE binary that calls nothing provider-bound, on purpose. The
    header promises that these functions do not link the ~120 KB Ed25519
    provider -- which is what lets sshd's launcher fingerprint the key a
    resident driver serves -- and a promise about what an image links is
    tested by an image: test_host_key_links_no_provider() asserts the
    provider is ABSENT here. AxlTestSsh, which calls axl_ssh_host_key_load(),
    is the control that reads it present.

    The fixtures are real ssh-keygen and openssl output (see
    axl-ssh-host-key-vectors.h), and the expected fingerprint and public
    line are what ssh-keygen itself printed for them, not values computed by
    the code under test.
**/

#include <axl.h>
#include "axl-test.h"
#include "axl-ssh-host-key-vectors.h"
#include "axl-ssh-host-key-internal.h"   /* axl_ssh_hk_stored_check, the seam */

/* The host-key seam this binary arms, or NULL: strong over the library's weak
   definition. Provider-free like everything else here. */
static const char *g_hk_fault;

bool
_axl_ssh_host_key_test_point(const char *point)
{
    return g_hk_fault != NULL && axl_strcmp(point, g_hk_fault) == 0;
}

/* The stored variable's private format, restated so the tests can plant
   one byte at a time: magic "AXK", version 1, the PKCS#8, the public key. */
#define HK_STORED_LEN  (4u + 48u + 32u)

static const uint8_t HK_PKCS8_PREFIX[16] = {
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
    0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20
};

static const AxlGuid HK_GUID = AXL_SSH_HOST_KEY_GUID;

/* A namespace over the host-key GUID, so a test can plant a variable there
   with whatever attributes it likes -- the library's own functions only
   ever write NV+BS. */
#define HK_NS  "hk-test"

/* HK_ED as the record the parser should produce. */
static void
hk_expected_record(AxlSshHostKeyRecord *rec)
{
    axl_memcpy(rec->pkcs8, HK_PKCS8_PREFIX, sizeof HK_PKCS8_PREFIX);
    axl_memcpy(rec->pkcs8 + sizeof HK_PKCS8_PREFIX, HK_ED_SEED, sizeof HK_ED_SEED);
    axl_memcpy(rec->public_key, HK_ED_PUBLIC, sizeof HK_ED_PUBLIC);
}

/* HK_ED in the stored format. */
static void
hk_stored_blob(uint8_t out[HK_STORED_LEN])
{
    AxlSshHostKeyRecord rec;
    hk_expected_record(&rec);
    out[0] = 'A';
    out[1] = 'X';
    out[2] = 'K';
    out[3] = 1;
    axl_memcpy(out + 4, rec.pkcs8, sizeof rec.pkcs8);
    axl_memcpy(out + 4 + sizeof rec.pkcs8, rec.public_key, sizeof rec.public_key);
}

static bool
record_is_zero(const AxlSshHostKeyRecord *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    for (size_t i = 0; i < sizeof *rec; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool
why_is(const char *why, const char *expect)
{
    return why != NULL && axl_strcmp(why, expect) == 0;
}

// ---------------------------------------------------------------------------
// T1: the provider-free half links no Ed25519 provider
// ---------------------------------------------------------------------------

static void
test_host_key_links_no_provider(void)
{
    /* Everything this binary calls is provider-free, so the Ed25519
       provider's weak reference must be unresolved here. AxlTestSsh, which
       also calls axl_ssh_host_key_load(), reads it true. */
    test_check(!axl_pk_alg_available(AXL_PK_ED25519),
               "host key: provider-free image links no Ed25519 provider");
}

// ---------------------------------------------------------------------------
// Fingerprint and public line
// ---------------------------------------------------------------------------

static void
test_host_key_fingerprint(void)
{
    char fp[AXL_SSH_FINGERPRINT_SIZE];
    char line[AXL_SSH_PUBLIC_LINE_SIZE];

    test_check(axl_ssh_host_key_fingerprint(HK_ED_PUBLIC, sizeof HK_ED_PUBLIC,
                                            fp, sizeof fp) == AXL_OK
               && axl_strcmp(fp, HK_ED_FINGERPRINT) == 0,
               "host key: fingerprint is ssh-keygen -lf's");
    test_check(axl_ssh_host_key_public_line(HK_ED_PUBLIC, sizeof HK_ED_PUBLIC,
                                            line, sizeof line) == AXL_OK
               && axl_strcmp(line, HK_ED_PUBLIC_LINE) == 0,
               "host key: public line is the .pub's");
    test_check(axl_strlen(HK_ED_FINGERPRINT) + 1 == AXL_SSH_FINGERPRINT_SIZE,
               "host key: AXL_SSH_FINGERPRINT_SIZE fits exactly");
    test_check(axl_strlen(HK_ED_PUBLIC_LINE) + 1 == AXL_SSH_PUBLIC_LINE_SIZE,
               "host key: AXL_SSH_PUBLIC_LINE_SIZE fits exactly");

    char small[AXL_SSH_FINGERPRINT_SIZE];
    axl_memset(small, 'Z', sizeof small);
    test_check(axl_ssh_host_key_fingerprint(HK_ED_PUBLIC, sizeof HK_ED_PUBLIC,
                                            small, sizeof small - 1) == AXL_INVALID
               && small[0] == 'Z',
               "host key: fingerprint refuses a short buffer, untouched");
    test_check(axl_ssh_host_key_fingerprint(HK_ED_PUBLIC, 31, fp, sizeof fp)
                   == AXL_INVALID,
               "host key: fingerprint refuses a 31-byte key");
    test_check(axl_ssh_host_key_fingerprint(NULL, 32, fp, sizeof fp) == AXL_INVALID,
               "host key: fingerprint refuses NULL");
    test_check(axl_ssh_host_key_public_line(HK_ED_PUBLIC, sizeof HK_ED_PUBLIC,
                                            line, sizeof line - 1) == AXL_INVALID,
               "host key: public line refuses a short buffer");
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/* Parse @a data and require AXL_OK with HK_ED's record. */
static void
parse_ok(const void *data, size_t len, const char *name)
{
    AxlSshHostKeyRecord got;
    AxlSshHostKeyRecord want;
    const char         *why = "unset";

    hk_expected_record(&want);
    int rc = axl_ssh_host_key_parse(data, len, &got, &why);
    test_check(rc == AXL_OK && axl_memcmp(&got, &want, sizeof got) == 0, name);
    axl_ssh_host_key_record_clear(&got);
}

/* Parse @a data and require exactly @a rc_want and @a why_want, with the
   record zeroed. */
static void
parse_refused(const void *data, size_t len, int rc_want, const char *why_want,
              const char *name)
{
    AxlSshHostKeyRecord got;
    const char         *why = NULL;

    axl_memset(&got, 0xA5, sizeof got);
    int rc = axl_ssh_host_key_parse(data, len, &got, &why);
    test_check(rc == rc_want && why_is(why, why_want) && record_is_zero(&got),
               name);
}

static void
test_host_key_parse_accepts(void)
{
    parse_ok(HK_ED, sizeof HK_ED, "parse: ssh-keygen's ed25519 key");
    parse_ok(HK_ED_CRLF, sizeof HK_ED_CRLF, "parse: CRLF line ends");
    parse_ok(HK_ED_BOM, sizeof HK_ED_BOM, "parse: UTF-8 with a BOM");
    parse_ok(HK_ED_UTF16, sizeof HK_ED_UTF16, "parse: UTF-16LE with a BOM");
    parse_ok(HK_ED_NO_FINAL_NL, sizeof HK_ED_NO_FINAL_NL,
             "parse: no newline after END");
    parse_ok(HK_ED_AFTER_END, sizeof HK_ED_AFTER_END,
             "parse: text after END is ignored");
    parse_ok(HK_ED_SPACES, sizeof HK_ED_SPACES,
             "parse: spaces and tabs inside the base64 are skipped, as b64_pton does");

    /* why is optional. */
    AxlSshHostKeyRecord got;
    test_check(axl_ssh_host_key_parse(HK_ED, sizeof HK_ED, &got, NULL) == AXL_OK,
               "parse: why may be NULL");
    axl_ssh_host_key_record_clear(&got);
    test_check(record_is_zero(&got), "record_clear: zeroes the record");
    axl_ssh_host_key_record_clear(NULL);
}

static void
test_host_key_parse_refuses(void)
{
    parse_refused(HK_ENC, sizeof HK_ENC, AXL_UNSUPPORTED,
                  "passphrase-protected host keys are not supported",
                  "parse: passphrase-protected key refused by name");
    parse_refused(HK_RSA, sizeof HK_RSA, AXL_UNSUPPORTED,
                  "ssh-rsa host keys are not supported; only ssh-ed25519",
                  "parse: ssh-rsa refused by name");
    parse_refused(HK_ECDSA, sizeof HK_ECDSA, AXL_UNSUPPORTED,
                  "ecdsa-sha2-nistp256 host keys are not supported; only ssh-ed25519",
                  "parse: ecdsa-sha2-nistp256 refused by name");
    parse_refused(HK_RSA_ENC, sizeof HK_RSA_ENC, AXL_UNSUPPORTED,
                  "ssh-rsa host keys are not supported; only ssh-ed25519",
                  "parse: an encrypted ssh-rsa is refused for its type");
    parse_refused(HK_CERT, sizeof HK_CERT, AXL_UNSUPPORTED,
                  "ssh-ed25519-cert-v01@openssh.com host keys are not supported;"
                  " only ssh-ed25519",
                  "parse: a certificate type refused by name");
    parse_refused(HK_UNKNOWN_TYPE, sizeof HK_UNKNOWN_TYPE, AXL_UNSUPPORTED,
                  "unknown host key type; only ssh-ed25519 is supported",
                  "parse: an unknown type");
    parse_refused(HK_PKCS8, sizeof HK_PKCS8, AXL_UNSUPPORTED,
                  "PKCS#8 keys are not supported; convert with"
                  " ssh-keygen -p -N '' -f FILE",
                  "parse: PKCS#8 PEM refused with the conversion");
    parse_refused(HK_ENC_PKCS8, sizeof HK_ENC_PKCS8, AXL_UNSUPPORTED,
                  "passphrase-protected host keys are not supported",
                  "parse: encrypted PKCS#8 PEM refused as passphrase-protected");
    parse_refused(HK_RSA_PEM, sizeof HK_RSA_PEM, AXL_UNSUPPORTED,
                  "ssh-rsa host keys are not supported; only ssh-ed25519",
                  "parse: a traditional RSA PEM refused by name");
    parse_refused(HK_EC_PEM, sizeof HK_EC_PEM, AXL_UNSUPPORTED,
                  "ecdsa host keys are not supported; only ssh-ed25519",
                  "parse: a traditional EC PEM refused by name");
    parse_refused(HK_NOT_KEY, sizeof HK_NOT_KEY, AXL_INVALID,
                  "not a private key file", "parse: not a key");
    parse_refused(HK_ED_BEFORE_BEGIN, sizeof HK_ED_BEFORE_BEGIN, AXL_INVALID,
                  "not a private key file", "parse: text before BEGIN refused");
    parse_refused(HK_NKEYS2, sizeof HK_NKEYS2, AXL_INVALID,
                  "more than one key in the file", "parse: two keys");
    parse_refused(HK_NKEYS0, sizeof HK_NKEYS0, AXL_INVALID,
                  "truncated or malformed key", "parse: zero keys");
    parse_refused(HK_CHECK_DIFF, sizeof HK_CHECK_DIFF, AXL_INVALID,
                  "corrupt key (check values differ)", "parse: check values differ");
    parse_refused(HK_BAD_PAD, sizeof HK_BAD_PAD, AXL_INVALID,
                  "corrupt key (bad padding)", "parse: bad padding");
    parse_refused(HK_ENVELOPE_DIFF, sizeof HK_ENVELOPE_DIFF, AXL_INVALID,
                  "corrupt key (public and private halves differ)",
                  "parse: envelope public key differs");
    parse_refused(HK_SECRET_PUB_DIFF, sizeof HK_SECRET_PUB_DIFF, AXL_INVALID,
                  "corrupt key (public and private halves differ)",
                  "parse: the secret's public half differs");
    parse_refused(HK_TRAILING, sizeof HK_TRAILING, AXL_INVALID,
                  "trailing data after the key", "parse: trailing data");
    parse_refused(HK_TRUNC, sizeof HK_TRUNC, AXL_INVALID,
                  "truncated or malformed key", "parse: truncated");
    parse_refused(HK_ENCLEN_ODD, sizeof HK_ENCLEN_ODD, AXL_INVALID,
                  "truncated or malformed key",
                  "parse: encrypted length not a multiple of 8");
    parse_refused(HK_KDFOPTS, sizeof HK_KDFOPTS, AXL_INVALID,
                  "truncated or malformed key", "parse: KDF options with KDF none");
    parse_refused(HK_BAD_MAGIC, sizeof HK_BAD_MAGIC, AXL_INVALID,
                  "truncated or malformed key", "parse: bad magic");
    parse_refused(HK_PK_LEN, sizeof HK_PK_LEN, AXL_INVALID,
                  "truncated or malformed key", "parse: a 31-byte public key");
    parse_refused(HK_NO_END, sizeof HK_NO_END, AXL_INVALID,
                  "truncated or malformed key", "parse: no END line");
    parse_refused(HK_SK_LEN, sizeof HK_SK_LEN, AXL_INVALID,
                  "truncated or malformed key", "parse: a 63-byte secret");
    parse_refused(HK_PTYPE, sizeof HK_PTYPE, AXL_INVALID,
                  "corrupt key (public and private halves differ)",
                  "parse: a private section of another type");
    parse_refused(HK_COMMENT_NUL, sizeof HK_COMMENT_NUL, AXL_INVALID,
                  "truncated or malformed key",
                  "parse: a comment with a NUL, as sshbuf_get_cstring refuses");

    AxlSshHostKeyRecord rec;
    test_check(axl_ssh_host_key_parse(NULL, 4, &rec, NULL) == AXL_INVALID,
               "parse: NULL data refused");
    test_check(axl_ssh_host_key_parse(HK_ED, sizeof HK_ED, NULL, NULL) == AXL_INVALID,
               "parse: NULL record refused");
}

/* AXL_SSH_HOST_KEY_FILE_MAX is on the bytes as stored, before decoding: a
   valid key padded after END to exactly the limit parses, one byte more is
   refused. */
static void
test_host_key_parse_size_limit(void)
{
    uint8_t *big = axl_malloc(AXL_SSH_HOST_KEY_FILE_MAX + 1);
    if (big == NULL) {
        test_skip_n(2, "parse: size limit (no memory)");
        return;
    }
    axl_memset(big, '#', AXL_SSH_HOST_KEY_FILE_MAX + 1);
    axl_memcpy(big, HK_ED, sizeof HK_ED);
    parse_ok(big, AXL_SSH_HOST_KEY_FILE_MAX, "parse: a file of exactly the limit");
    parse_refused(big, AXL_SSH_HOST_KEY_FILE_MAX + 1, AXL_INVALID,
                  "file too large to be a host key", "parse: one byte over the limit");
    axl_free(big);
}

// ---------------------------------------------------------------------------
// File reader
// ---------------------------------------------------------------------------

#define HK_FILE  "fs0:\\axl-hk-test.key"

static void
test_host_key_read_file(void)
{
    AxlSshHostKeyRecord got;
    AxlSshHostKeyRecord want;
    const char         *why = NULL;

    hk_expected_record(&want);
    if (axl_file_set_contents(HK_FILE, HK_ED_CRLF, sizeof HK_ED_CRLF) != AXL_OK) {
        test_skip_n(5, "read_file: cannot write a fixture to fs0:");
        return;
    }
    test_check(axl_ssh_host_key_read_file(HK_FILE, &got, &why) == AXL_OK
               && axl_memcmp(&got, &want, sizeof got) == 0,
               "read_file: reads and parses the file");
    axl_ssh_host_key_record_clear(&got);

    test_check(axl_file_set_contents(HK_FILE, HK_RSA, sizeof HK_RSA) == AXL_OK
               && axl_ssh_host_key_read_file(HK_FILE, &got, &why) == AXL_UNSUPPORTED
               && why_is(why, "ssh-rsa host keys are not supported; only ssh-ed25519"),
               "read_file: passes the parser's refusal through");

    /* Refused by its SIZE, before anything is read. */
    uint8_t *big = axl_malloc(AXL_SSH_HOST_KEY_FILE_MAX + 1);
    if (big != NULL) {
        axl_memset(big, '#', AXL_SSH_HOST_KEY_FILE_MAX + 1);
        axl_memcpy(big, HK_ED, sizeof HK_ED);
    }
    test_check(big != NULL
               && axl_file_set_contents(HK_FILE, big, AXL_SSH_HOST_KEY_FILE_MAX + 1)
                      == AXL_OK
               && axl_ssh_host_key_read_file(HK_FILE, &got, &why) == AXL_INVALID
               && why_is(why, "file too large to be a host key"),
               "read_file: a file over the limit refused");
    axl_free(big);
    (void)axl_file_delete(HK_FILE);

    test_check(axl_ssh_host_key_read_file(HK_FILE, &got, &why) == AXL_NOT_FOUND
               && why_is(why, "no such file"),
               "read_file: a missing file is NOT_FOUND");
    test_check(axl_ssh_host_key_read_file("fs0:\\", &got, &why) == AXL_NOT_FOUND
               && why_is(why, "no such file"),
               "read_file: a directory is NOT_FOUND");
}

// ---------------------------------------------------------------------------
// The default variable
// ---------------------------------------------------------------------------

/* The raw bytes now stored under the host-key name, or NULL. */
static void *
hk_raw(size_t *size, uint32_t *attrs)
{
    void *data = NULL;
    *size = 0;
    if (axl_var_read(AXL_SSH_HOST_KEY_VAR, &HK_GUID, attrs, &data, size) != AXL_OK) {
        return NULL;
    }
    return data;
}

/* Plant @a len bytes under the host-key name with @a flags (AXL_NV_*). */
static bool
hk_plant(const void *data, size_t len, uint32_t flags)
{
    (void)axl_nvstore_delete(HK_NS, AXL_SSH_HOST_KEY_VAR);
    return axl_nvstore_set(HK_NS, AXL_SSH_HOST_KEY_VAR, data, len, flags) == AXL_OK;
}

static void
nv_read_refused(const char *why_want, const char *name)
{
    AxlSshHostKeyRecord got;
    const char         *why = NULL;

    axl_memset(&got, 0xA5, sizeof got);
    test_check(axl_ssh_host_key_nv_read(&got, &why) == AXL_ERR
               && why_is(why, why_want) && record_is_zero(&got), name);
}

static void
test_host_key_nv(void)
{
    AxlSshHostKeyRecord rec;
    AxlSshHostKeyRecord got;
    const char         *why = NULL;
    uint8_t             blob[HK_STORED_LEN];

    test_check(axl_nvstore_register_namespace(HK_NS, &HK_GUID) == AXL_OK,
               "nv: test namespace over the host-key GUID");

    /* Absent, and deleting the absent is not an error (--regenerate on a
       machine that never had a key). */
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK,
               "nv_delete: an absent key is AXL_OK");
    test_check(axl_ssh_host_key_nv_read(&got, &why) == AXL_NOT_FOUND,
               "nv_read: absent is NOT_FOUND");

    /* Write, then read back the same record. */
    hk_expected_record(&rec);
    test_check(axl_ssh_host_key_nv_write(&rec, &why) == AXL_OK,
               "nv_write: stores a key");
    test_check(axl_ssh_host_key_nv_read(&got, &why) == AXL_OK
               && axl_memcmp(&got, &rec, sizeof got) == 0,
               "nv_read: reads back what nv_write stored");
    axl_ssh_host_key_record_clear(&got);

    /* The stored bytes are the documented private format, NV+BS. */
    size_t   size  = 0;
    uint32_t attrs = 0;
    void    *raw   = hk_raw(&size, &attrs);
    hk_stored_blob(blob);
    test_check(raw != NULL && size == HK_STORED_LEN
               && axl_memcmp(raw, blob, HK_STORED_LEN) == 0,
               "nv_write: stores magic, version, PKCS#8 and public key");
    test_check(attrs == (AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "nv_write: stores non-volatile, boot-services only");
    axl_free(raw);

    /* A stored key is never replaced by a write. */
    test_check(axl_ssh_host_key_nv_write(&rec, &why) == AXL_EXISTS
               && why_is(why, "a host key is already stored"),
               "nv_write: refuses to replace a stored key");

    test_check(axl_ssh_host_key_nv_delete() == AXL_OK
               && axl_ssh_host_key_nv_read(&got, &why) == AXL_NOT_FOUND,
               "nv_delete: deletes a stored key");

    /* A record whose private half is not Ed25519 is not stored. */
    rec.pkcs8[9] ^= 1;
    test_check(axl_ssh_host_key_nv_write(&rec, &why) == AXL_INVALID
               && why_is(why, "the key is not an Ed25519 key"),
               "nv_write: refuses a non-Ed25519 record");
    hk_expected_record(&rec);

    /* Planted variables: each refused by name, never read. */
    hk_stored_blob(blob);
    test_check(hk_plant(blob, sizeof blob,
                        AXL_NV_PERSISTENT | AXL_NV_BOOT | AXL_NV_RUNTIME),
               "nv: planted a runtime-access key");
    nv_read_refused("the stored key is readable by the OS (runtime access)",
                    "nv_read: refuses a runtime-access variable");
    test_check(axl_ssh_host_key_nv_write(&rec, &why) == AXL_EXISTS,
               "nv_write: refuses to replace a planted variable");
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK,
               "nv_delete: deletes a runtime-access variable");

    test_check(hk_plant(blob, sizeof blob, AXL_NV_BOOT),
               "nv: planted a volatile key");
    nv_read_refused("the stored key has the wrong attributes (not NV+BS)",
                    "nv_read: refuses a volatile variable");

    test_check(hk_plant(blob, sizeof blob - 1, AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "nv: planted a short key");
    nv_read_refused("the stored key has the wrong size", "nv_read: refuses a short value");

    uint8_t bad[HK_STORED_LEN];
    axl_memcpy(bad, blob, sizeof bad);
    bad[0] = 'B';
    test_check(hk_plant(bad, sizeof bad, AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "nv: planted a key with a wrong magic");
    nv_read_refused("the stored key is not in AxlSsh's format",
                    "nv_read: refuses a wrong magic");

    axl_memcpy(bad, blob, sizeof bad);
    bad[3] = 2;
    test_check(hk_plant(bad, sizeof bad, AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "nv: planted a key of format version 2");
    nv_read_refused("the stored key's format version is not supported",
                    "nv_read: refuses an unknown version");

    axl_memcpy(bad, blob, sizeof bad);
    bad[4 + 9] ^= 1;   /* inside the PKCS#8 algorithm OID */
    test_check(hk_plant(bad, sizeof bad, AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "nv: planted a key that is not Ed25519");
    nv_read_refused("the stored key is not an Ed25519 key",
                    "nv_read: refuses a non-Ed25519 PKCS#8");

    test_check(axl_ssh_host_key_nv_delete() == AXL_OK
               && axl_ssh_host_key_nv_read(&got, &why) == AXL_NOT_FOUND,
               "nv: cleaned up");

    test_check(axl_ssh_host_key_nv_read(NULL, &why) == AXL_INVALID,
               "nv_read: NULL record refused");
    test_check(axl_ssh_host_key_nv_write(NULL, &why) == AXL_INVALID,
               "nv_write: NULL record refused");
}

/* The raw attribute words OVMF cannot create -- an authenticated variable
   needs a signed payload -- graded against the pure check nv_read uses
   (midpoint review MP5). The values are EFI_VARIABLE_* bits. */
static void
test_host_key_stored_attrs(void)
{
    static const struct {
        uint32_t    attrs;
        const char *why;
    } cases[] = {
        { 0x13u, "the stored key is an authenticated variable; only its signer"
                 " can delete it" },                       /* NV|BS|AUTH_WRITE */
        { 0x23u, "the stored key is an authenticated variable; only its signer"
                 " can delete it" },                       /* NV|BS|TIME_AUTH  */
        { 0x83u, "the stored key is an authenticated variable; only its signer"
                 " can delete it" },                       /* NV|BS|ENHANCED   */
        { 0x27u, "the stored key is an authenticated variable; only its signer"
                 " can delete it" },                       /* ...ahead of RT   */
        { 0x07u, "the stored key is readable by the OS (runtime access)" },
        { 0x0Bu, "the stored key has the wrong attributes (not NV+BS)" }, /* HW_ERR */
        { 0x43u, "the stored key has the wrong attributes (not NV+BS)" }, /* APPEND */
        { 0x02u, "the stored key has the wrong attributes (not NV+BS)" }, /* BS only */
    };
    uint8_t             blob[HK_STORED_LEN];
    AxlSshHostKeyRecord got;
    unsigned            wrong = 0;

    hk_stored_blob(blob);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *why = axl_ssh_hk_stored_check(cases[i].attrs, blob, sizeof blob,
                                                  &got);
        if (!why_is(why, cases[i].why)) {
            axl_printf("  attrs 0x%02x: got \"%s\"\n", (unsigned)cases[i].attrs,
                       why != NULL ? why : "(accepted)");
            wrong++;
        }
    }
    test_check(wrong == 0, "stored check: every raw attribute word refused by name");
    test_check(axl_ssh_hk_stored_check(0x03u, blob, sizeof blob, &got) == NULL,
               "stored check: exactly NV+BS is accepted");
    axl_ssh_host_key_record_clear(&got);
}

/* nv_delete's DENIED path: the firmware refusing an unsigned delete, as it
   does for an authenticated variable. Nothing is deleted. */
static void
test_host_key_delete_denied(void)
{
    AxlSshHostKeyRecord rec;
    hk_expected_record(&rec);
    test_check(axl_ssh_host_key_nv_write(&rec, NULL) == AXL_OK,
               "delete denied: a key is stored");
    g_hk_fault = "nv-delete-denied";
    int rc = axl_ssh_host_key_nv_delete();
    g_hk_fault = NULL;
    AxlSshHostKeyRecord got;
    test_check(rc == AXL_DENIED && axl_ssh_host_key_nv_read(&got, NULL) == AXL_OK,
               "delete denied: AXL_DENIED, and the key is still there");
    axl_ssh_host_key_record_clear(&got);
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK, "delete denied: cleaned up");
}

static int
test_ssh_host_key_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlSshHostKey");

    test_host_key_links_no_provider();
    test_host_key_fingerprint();
    test_host_key_parse_accepts();
    test_host_key_parse_refuses();
    test_host_key_parse_size_limit();
    test_host_key_read_file();
    test_host_key_nv();
    test_host_key_stored_attrs();
    test_host_key_delete_denied();

    return test_print_results();
}

AXL_APP(test_ssh_host_key_main)
