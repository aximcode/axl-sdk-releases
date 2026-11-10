/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-auth.c
    The authorized_keys store and its line parser.

    The parser is the whole security surface of this file: it decides which
    32 bytes may log in. It is therefore strict rather than forgiving. Every
    lenience an authorized_keys parser could offer -- ignoring an options
    prefix, accepting a second algorithm, tolerating trailing bytes inside
    the blob -- is a way to authorize a key whose author meant something
    narrower. Each refusal below has a test.
**/

#include "axl-ssh-auth.h"
#include "axl-ssh-internal.h"
#include <axl/axl-crypto.h>
#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-ssh-core.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>

/** The key type name. Same string as #AXL_SSH_HOSTKEY_NAME, and the same
    thing: RFC 8709 names ONE algorithm, "ssh-ed25519", used for host keys and
    for user keys alike. Reusing the macro is not a coincidence to be
    untangled later. */
#define KEY_TYPE      AXL_SSH_HOSTKEY_NAME
#define KEY_TYPE_LEN  (sizeof KEY_TYPE - 1)

static bool
is_field_space(char ch)
{
    /* Not axl_isspace: a newline must never be treated as an intra-line
       separator, or a caller that handed us two lines would parse a key from
       one and a comment from the other. The loader splits on newlines and
       this function must not undo that.

       '\r' IS treated as a separator, and that is load-bearing rather than
       merely tolerant: axl_ssh_server_load_authorized_keys splits its file
       on '\n' only, so a CRLF-terminated authorized_keys -- the only line
       ending a FAT ESP editor produces on most platforms -- hands every line
       here with a trailing '\r' still attached. Without this, that '\r'
       would land inside the trailing comment field on every line, which is
       harmless, or inside the base64 field itself if a line has no comment,
       which is not: axl_base64_decode would refuse it and the whole key
       would be rejected. Treating '\r' as a separator here is what makes a
       CRLF file work at all. */
    return ch == ' ' || ch == '\t' || ch == '\r';
}

int
axl_ssh_authkey_parse(const char *line, size_t len, uint8_t *out32)
{
    if (line == NULL || out32 == NULL) {
        return AXL_INVALID;
    }

    size_t i = 0;
    while (i < len && is_field_space(line[i])) {
        i++;
    }
    if (i == len || line[i] == '#') {
        return AXL_NOT_FOUND;
    }

    /* Field 1, compared WHOLE. An options prefix lands here as its own field
       and fails to compare -- which is the intended refusal, not an accident
       of the tokenizer. */
    size_t type_start = i;
    while (i < len && !is_field_space(line[i])) {
        i++;
    }
    size_t type_len = i - type_start;
    /* The length test MUST stay first: axl_memcmp reads a fixed
       KEY_TYPE_LEN bytes, and only the short-circuit guarantees those bytes
       are inside the line. Reordering these two clauses, or extracting the
       compare into a helper called unconditionally, reintroduces an
       out-of-bounds read on a short trailing field. */
    if (type_len != KEY_TYPE_LEN
        || axl_memcmp(line + type_start, KEY_TYPE, KEY_TYPE_LEN) != 0) {
        return AXL_INVALID;
    }

    while (i < len && is_field_space(line[i])) {
        i++;
    }
    size_t b64_start = i;
    while (i < len && !is_field_space(line[i])) {
        i++;
    }
    size_t b64_len = i - b64_start;
    if (b64_len == 0 || b64_len > AXL_SSH_AUTHKEY_B64_MAX) {
        return AXL_INVALID;
    }
    /* Anything after this is the comment field and is ignored. */

    /* axl_base64_decode wants a NUL-terminated string and the field ends at a
       space, so it is copied out -- bounded above before a byte moves. */
    char b64[AXL_SSH_AUTHKEY_B64_MAX + 1];
    axl_memcpy(b64, line + b64_start, b64_len);
    b64[b64_len] = '\0';

    void  *raw     = NULL;
    size_t raw_len = 0;
    if (axl_base64_decode(b64, &raw, &raw_len) != AXL_OK) {
        return AXL_INVALID;
    }

    int rc = axl_ssh_hostkey_blob_parse((const uint8_t *)raw, raw_len, out32);
    axl_free(raw);
    return rc == AXL_OK ? AXL_OK : AXL_INVALID;
}

int
axl_ssh_authkeys_add_line(AxlSshAuthKeys *ks, const char *line, size_t len)
{
    if (ks == NULL) {
        return AXL_INVALID;
    }

    uint8_t pub[AXL_SSH_ED25519_PUB_LEN];
    int     rc = axl_ssh_authkey_parse(line, len, pub);
    if (rc != AXL_OK) {
        return rc;
    }

    /* A file that lists a key twice must not spend two of sixteen slots. */
    if (axl_ssh_authkeys_contains(ks, pub)) {
        return AXL_OK;
    }
    if (ks->n >= AXL_SSH_MAX_AUTHORIZED_KEYS) {
        return AXL_NO_RESOURCES;
    }
    axl_memcpy(ks->key[ks->n], pub, sizeof pub);
    ks->n++;
    return AXL_OK;
}

bool
axl_ssh_authkeys_contains(const AxlSshAuthKeys *ks, const uint8_t *pub32)
{
    if (ks == NULL || pub32 == NULL) {
        return false;
    }
    for (unsigned i = 0; i < ks->n; i++) {
        if (axl_memcmp(ks->key[i], pub32, AXL_SSH_ED25519_PUB_LEN) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t
axl_ssh_fail_delay_target_us(const uint8_t *secret, size_t secret_len,
                             const char *user, size_t user_len)
{
    /* The largest value the per-user term can produce: returned for every
       user on every failure path, so no failure is distinguishable by
       timing from another user's. */
    const uint32_t fallback = AXL_SSH_FAIL_DELAY_MIN_US + 4294u;

    if (secret == NULL || (user == NULL && user_len != 0)) {
        return fallback;
    }
    AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_SHA512);
    if (cs == NULL) {
        return fallback;
    }
    uint8_t digest[64];
    size_t  n = sizeof digest;
    axl_checksum_update(cs, secret, secret_len);
    if (user_len != 0) {
        axl_checksum_update(cs, user, user_len);
    }
    axl_checksum_get_digest(cs, digest, &n);
    axl_checksum_free(cs);
    if (n < 4) {
        axl_ssh_secure_zero(digest, sizeof digest);
        return fallback;
    }
    uint32_t be32 = ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16)
                  | ((uint32_t)digest[2] << 8) | (uint32_t)digest[3];
    /* Keyed by the timing secret: knowing digests is knowing every user's
       delay, which is what the secret keeps private. */
    axl_ssh_secure_zero(digest, sizeof digest);
    /* OpenSSH: PEEK_U32(hash) / 1000 / 1000 / 1000 / 1000 SECONDS. The same
       quantity in microseconds is be32 / 10^6, i.e. 0..4294. */
    return AXL_SSH_FAIL_DELAY_MIN_US + be32 / 1000000u;
}

uint64_t
axl_ssh_fail_reply_delay_us(uint64_t elapsed_us, uint64_t target_us)
{
    if (elapsed_us > AXL_SSH_FAIL_DELAY_MAX_US || target_us == 0) {
        return 0;
    }
    /* OpenSSH: while ((remain = seconds - elapsed) < 0.0) seconds *= 2;
       -- strictly less, so a request finishing exactly on target is not
       doubled. elapsed <= 5e6 bounds target below 1e7: no overflow. */
    while (target_us < elapsed_us) {
        target_us *= 2;
    }
    return target_us - elapsed_us;
}

bool
axl_ssh_listen_has_method(unsigned authorized_keys, bool have_password)
{
    return authorized_keys > 0 || have_password;
}

bool
axl_ssh_password_reaches_hook(bool change, const uint8_t *pw, uint32_t pw_len)
{
    if (change || pw_len == 0 || pw_len > AXL_SSH_AUTH_PASSWORD_MAX) {
        return false;
    }
    return axl_memchr(pw, 0, pw_len) == NULL;
}

bool
axl_ssh_auth_log_due(AxlSshAuthLogRate *r, uint64_t now, bool have_now,
                     uint32_t *suppressed)
{
    /* The window has elapsed only when a readable clock SAYS so. now is
       monotonic, so now < noted_ms can only be a bad reading; treating it as
       elapsed would let one backwards step release a line early, and a clock
       that keeps stepping backwards would release every one. */
    bool due = !r->noted
               || (have_now && now >= r->noted_ms
                   && now - r->noted_ms >= AXL_SSH_AUTH_LOG_WINDOW_MS);
    if (!due) {
        if (r->suppressed < 0xFFFFFFFFu) {
            r->suppressed++;   /* saturates: a wrapped count reads as calm */
        }
        return false;
    }
    if (suppressed != NULL) {
        *suppressed = r->suppressed;
    }
    r->suppressed = 0;
    r->noted      = true;
    /* With no reading there is no window to anchor, so the window is
       anchored at zero -- the oldest instant the monotonic clock has. A
       later READABLE failure is then due once that clock reads a full
       window, which on any server that has been up a minute is at once, and
       within the first minute of uptime is when it reaches the window. NOT
       "due at once" unconditionally, which is what this comment used to
       claim: below AXL_SSH_AUTH_LOG_WINDOW_MS of uptime the subtraction
       says the window has not elapsed, and it is right to.

       Either way nothing is lost and nothing floods: the failure that
       could not be timed was itself announced (the `!r->noted` arm above),
       and every one suppressed in the meantime is counted and reported by
       the next line that does go out. */
    r->noted_ms   = have_now ? now : 0;
    return true;
}

AxlSshAuthRequest
axl_ssh_auth_request_next(AxlSshAuthRequest *last)
{
    if (++*last == 0) {
        *last = 1;   /* 0 is the invalid handle; skip it on wrap */
    }
    return *last;
}

void
axl_ssh_format_user_change(char *out, size_t out_size, const char *was,
                           const char *now, const char *peer)
{
    axl_snprintf(out, out_size, AXL_SSH_USER_CHANGE_FMT, was, peer, was,
                 now);
}

void
axl_ssh_format_timeout(char *out, size_t out_size, const char *peer,
                       bool was_queued, bool hook_running, bool have_waited,
                       uint64_t waited_ms)
{
    /* Empty unless the connection was queued, so the line a peer that simply
       said nothing produces stays byte-for-byte what it always was. */
    char tail[AXL_SSH_TIMEOUT_TAIL_MAX];

    tail[0] = '\0';
    if (was_queued) {
        const char *where = hook_running ? AXL_SSH_TIMEOUT_WHERE_HOOK
                                         : AXL_SSH_TIMEOUT_WHERE_QUEUE;
        if (have_waited) {
            axl_snprintf(tail, sizeof tail, AXL_SSH_TIMEOUT_TAIL_FMT,
                         (unsigned long long)waited_ms, where);
        } else {
            axl_snprintf(tail, sizeof tail, AXL_SSH_TIMEOUT_TAIL_NO_MS_FMT,
                         where);
        }
    }
    axl_snprintf(out, out_size, AXL_SSH_TIMEOUT_FMT, peer, tail);
}

void
axl_ssh_format_hook_hold(char *out, size_t out_size, bool stalled,
                         bool is_password, uint64_t held_ms, unsigned waiting,
                         const char *user, const char *peer)
{
    axl_snprintf(out, out_size,
                 stalled ? AXL_SSH_HOOK_STALLED_FMT : AXL_SSH_HOOK_SLOW_FMT,
                 is_password ? "password" : "publickey",
                 (unsigned long long)held_ms, waiting, user, peer);
}

bool
axl_ssh_userauth_user_changed(const uint8_t *kept, size_t kept_len,
                              uint32_t kept_wire_len, const uint8_t *user,
                              uint32_t user_len)
{
    /* The length first: it settles every prefix case, and past the store it
       is the only part of the old name still known in full. */
    if (user_len != kept_wire_len) {
        return true;
    }
    size_t n = kept_len < user_len ? kept_len : user_len;
    return n != 0 && axl_memcmp(kept, user, n) != 0;
}

AxlSshSigCheck
axl_ssh_userauth_sig_check(const uint8_t *session_id, size_t sid_len,
                           const uint8_t *pub, const uint8_t *req,
                           size_t req_len, size_t signed_len)
{
    if (session_id == NULL || pub == NULL || req == NULL
        || signed_len > req_len) {
        return AXL_SSH_SIG_UNCHECKED;
    }

    /* The peer's half FIRST, and with nothing allocated: a signature field
       that is missing, truncated or not an ssh-ed25519 blob is the peer's
       doing whatever the memory state, and deciding it here means no
       allocation failure below can excuse it. */
    size_t         off          = signed_len;
    const uint8_t *sig_blob     = NULL;
    uint32_t       sig_blob_len = 0;
    uint8_t        sig[AXL_SSH_ED25519_SIG_LEN];
    if (axl_ssh_get_string(req, req_len, &off, &sig_blob, &sig_blob_len)
            != AXL_OK
        || axl_ssh_signature_blob_parse(sig_blob, sig_blob_len, sig)
               != AXL_OK) {
        return AXL_SSH_SIG_FORGED;
    }

    /* Ours from here until the verdict. Each of these three fails only for
       lack of memory: axl_ssh_userauth_signed_blob's other refusal is a NULL
       argument, excluded above, and the Ed25519 provider's raw import checks
       nothing but the length, which is fixed -- a bad point is refused by
       the verify, as a verdict. */
    AxlSshSigCheck result      = AXL_SSH_SIG_UNCHECKED;
    AxlString     *signed_data = axl_string_new("");
    AxlPkKey      *key         = axl_pk_key_from_raw_public(
                                     AXL_PK_ED25519, pub,
                                     AXL_SSH_ED25519_PUB_LEN);
    if (signed_data != NULL && key != NULL
        && axl_ssh_userauth_signed_blob(signed_data, session_id, sid_len,
                                        req, signed_len) == AXL_OK) {
        int rc = axl_pk_key_verify(
            key, (const uint8_t *)axl_string_data(signed_data),
            axl_string_len(signed_data), AXL_PK_SIG_RAW, sig, sizeof sig);
        /* AXL_NO_RESOURCES is the verify's own allocation failing before
           the signature was examined (axl_pk_key_verify's contract); every
           other refusal is a verdict on the signature. */
        if (rc == AXL_OK) {
            result = AXL_SSH_SIG_VALID;
        } else if (rc != AXL_NO_RESOURCES) {
            result = AXL_SSH_SIG_FORGED;
        }
    }
    axl_pk_key_free(key);
    /* string(session_id) || the signed request. The session id is
       SHA256(... || mpint(K)) -- not a public transcript value -- so it goes
       back to the pool scrubbed, like every other buffer that has held a
       hash input containing K. */
    axl_ssh_string_free_secure(signed_data);
    return result;
}
