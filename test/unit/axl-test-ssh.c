/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-test-ssh.c
    Test application for AxlSsh — wire codec, packet framing, kex KDF.
**/

#include <axl.h>
#include "axl-test.h"                 /* test_check / test_print_results */
#include <axl/axl-string.h>           /* AxlString builder */
#include <axl/axl-str.h>              /* axl_memcmp */
#include <axl/axl-crypto.h>
#include "../../src/net/axl-ssh-admit.h"
#include "../../src/net/axl-ssh-internal.h"
#include "../../src/net/axl-ssh-auth.h"
#include "../../src/net/axl-ssh-reap.h"
#include "../../src/net/axl-ssh-penalty.h"
#include "../../src/net/axl-ssh-chan.h"
#include "../../src/net/axl-ssh-rekey.h"
#include "../../src/net/axl-ssh-idle.h"
#include "axl-ssh-host-key-vectors.h"

static void
test_ssh_put_u32(void)
{
    AxlString *b = axl_string_new(NULL);
    test_check(axl_ssh_put_u32(b, 0x01020304u) == AXL_OK, "ssh put_u32: ok");
    test_check(axl_string_len(b) == 4, "ssh put_u32: 4 bytes");
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    test_check(d[0] == 0x01 && d[1] == 0x02 && d[2] == 0x03 && d[3] == 0x04,
               "ssh put_u32: big-endian");
    axl_string_free(b);
}

static void
test_ssh_get_u32(void)
{
    const uint8_t d[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t off = 0; uint32_t v = 0;
    test_check(axl_ssh_get_u32(d, sizeof d, &off, &v) == AXL_OK, "ssh get_u32: ok");
    test_check(v == 0xDEADBEEFu, "ssh get_u32: value");
    test_check(off == 4, "ssh get_u32: advanced 4");

    /* Truncated input must fail AND leave off untouched. */
    size_t off2 = 0;
    test_check(axl_ssh_get_u32(d, 3, &off2, &v) == AXL_ERR, "ssh get_u32: short input errs");
    test_check(off2 == 0, "ssh get_u32: off unchanged on error");

    /* An offset PAST the end must be refused, not read. `len - *off` underflows
       for this input, so a bounds check written in the wrong order reads out of
       bounds instead of erroring. */
    size_t off3 = 8;
    test_check(axl_ssh_get_u32(d, sizeof d, &off3, &v) == AXL_ERR,
               "ssh get_u32: offset past end errs");
    test_check(off3 == 8, "ssh get_u32: off unchanged when past end");
}

static void
test_ssh_string_roundtrip(void)
{
    AxlString *b = axl_string_new(NULL);
    test_check(axl_ssh_put_string(b, "ssh-ed25519", 11) == AXL_OK, "ssh put_string: ok");
    test_check(axl_string_len(b) == 4 + 11, "ssh put_string: len prefix + body");

    size_t off = 0; const uint8_t *s = NULL; uint32_t n = 0;
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    test_check(axl_ssh_get_string(d, axl_string_len(b), &off, &s, &n) == AXL_OK,
               "ssh get_string: ok");
    test_check(n == 11, "ssh get_string: length");
    test_check(axl_memcmp(s, "ssh-ed25519", 11) == 0, "ssh get_string: bytes");
    test_check(off == 4 + 11, "ssh get_string: advanced past the whole field");
    axl_string_free(b);
}

static void
test_ssh_string_length_lies(void)
{
    /* A length field larger than the buffer is the classic parser bug. */
    const uint8_t d[8] = { 0x00, 0x00, 0x10, 0x00, 'a', 'b', 'c', 'd' };
    size_t off = 0; const uint8_t *s = NULL; uint32_t n = 0;
    test_check(axl_ssh_get_string(d, sizeof d, &off, &s, &n) == AXL_ERR,
               "ssh get_string: length past end is refused");
    test_check(off == 0, "ssh get_string: off unchanged on error");

    /* Exactly-fits is the boundary on the other side of that check, and must
       be ACCEPTED — a guard written with the wrong comparison rejects it. */
    const uint8_t e[8] = { 0x00, 0x00, 0x00, 0x04, 'a', 'b', 'c', 'd' };
    size_t off2 = 0;
    test_check(axl_ssh_get_string(e, sizeof e, &off2, &s, &n) == AXL_OK,
               "ssh get_string: exactly-fitting length is accepted");
    test_check(n == 4 && axl_memcmp(s, "abcd", 4) == 0, "ssh get_string: boundary bytes");
    test_check(off2 == 8, "ssh get_string: boundary advanced to end");
}

static void
test_ssh_codec_null_args(void)
{
    /* Every reader takes four pointers from a caller that may be mid-refactor.
       These are the safe negatives: no firmware call, no allocation, no state. */
    const uint8_t d[8] = { 0x00, 0x00, 0x00, 0x04, 'a', 'b', 'c', 'd' };
    size_t off = 0; const uint8_t *s = NULL; uint32_t n = 0; uint32_t v = 0;

    test_check(axl_ssh_get_u32(NULL, 4, &off, &v) == AXL_ERR, "ssh get_u32: NULL buf errs");
    test_check(axl_ssh_get_u32(d, 4, NULL, &v) == AXL_ERR, "ssh get_u32: NULL off errs");
    test_check(axl_ssh_get_u32(d, 4, &off, NULL) == AXL_ERR, "ssh get_u32: NULL out errs");

    test_check(axl_ssh_get_string(NULL, 8, &off, &s, &n) == AXL_ERR,
               "ssh get_string: NULL buf errs");
    test_check(axl_ssh_get_string(d, 8, NULL, &s, &n) == AXL_ERR,
               "ssh get_string: NULL off errs");
    test_check(axl_ssh_get_string(d, 8, &off, NULL, &n) == AXL_ERR,
               "ssh get_string: NULL out errs");
    test_check(axl_ssh_get_string(d, 8, &off, &s, NULL) == AXL_ERR,
               "ssh get_string: NULL out_len errs");
}

static void
test_ssh_ident_parse(void)
{
    const char *ok = "SSH-2.0-OpenSSH_9.6\r\n";
    size_t end = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)ok, axl_strlen(ok), &end) == AXL_OK,
               "ssh ident: accepts SSH-2.0");
    test_check(end == axl_strlen(ok), "ssh ident: consumes the CRLF");

    /* RFC 4253 section 4.2 allows arbitrary lines BEFORE the identification. */
    const char *pre = "hello\r\nSSH-2.0-OpenSSH_9.6\r\n";
    size_t end2 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)pre, axl_strlen(pre), &end2) == AXL_OK,
               "ssh ident: skips preamble lines");
    test_check(end2 == axl_strlen(pre), "ssh ident: consumes through the ident line");

    /* Incomplete must be distinguishable from malformed. */
    const char *partial = "SSH-2.0-Open";
    size_t end3 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)partial, axl_strlen(partial), &end3)
               == AXL_INCOMPLETE, "ssh ident: partial line is INCOMPLETE not ERR");

    /* SSH-1.x is refused outright. */
    const char *v1 = "SSH-1.5-OpenSSH\r\n";
    size_t end4 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)v1, axl_strlen(v1), &end4) == AXL_ERR,
               "ssh ident: SSH-1.x refused");

    /* RFC 4253 section 4.2 caps the line at 255 bytes including CRLF. */
    char big[300];
    axl_memset(big, 'x', sizeof big);
    axl_memcpy(big, "SSH-2.0-", 8);
    big[298] = '\r'; big[299] = '\n';
    size_t end5 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)big, sizeof big, &end5) == AXL_ERR,
               "ssh ident: over-long line refused");

    test_check(axl_ssh_parse_ident(NULL, 4, &end5) == AXL_ERR, "ssh ident: NULL buf errs");
    test_check(axl_ssh_parse_ident((const uint8_t *)ok, axl_strlen(ok), NULL) == AXL_ERR,
               "ssh ident: NULL end_off errs");
}

static void
test_ssh_ident_unterminated_is_bounded(void)
{
    /* THE DoS, and the reason "no CRLF yet" cannot simply mean "send more":
       a line that is ALREADY too long to ever be a legal ident can never
       become one, so waiting for its terminator is waiting forever. The cap
       must apply to an UNTERMINATED line, not only to a complete one.

       Boundary: a legal line is <= 255 bytes INCLUDING CRLF, so 253 bytes of
       content can still complete, and 254 cannot. */
    char buf[260];
    axl_memset(buf, 'x', sizeof buf);
    axl_memcpy(buf, "SSH-2.0-", 8);
    size_t end = 0;

    test_check(axl_ssh_parse_ident((const uint8_t *)buf, 253, &end) == AXL_INCOMPLETE,
               "ssh ident: 253 unterminated bytes can still complete");
    test_check(axl_ssh_parse_ident((const uint8_t *)buf, 254, &end) == AXL_ERR,
               "ssh ident: 254 unterminated bytes can never be legal");

    /* A trailing CR is the first half of a terminator, not content, so it must
       not count against the cap. 253 content + CR is still completable. */
    buf[253] = '\r';
    test_check(axl_ssh_parse_ident((const uint8_t *)buf, 254, &end) == AXL_INCOMPLETE,
               "ssh ident: a trailing CR is a terminator half, not content");
}

static void
test_ssh_ident_preamble_is_bounded(void)
{
    /* The per-line cap alone still lets a peer stream unlimited SHORT preamble
       lines and keep us scanning and buffering forever. Total preamble is
       capped too. */
    size_t n = AXL_SSH_PREAMBLE_MAX + 300;
    char *flood = axl_malloc(n);
    if (flood == NULL) {
        test_skip_n(2, "ssh ident: preamble flood (allocation failed)");
        return;
    }
    for (size_t i = 0; i + 2 < n; i += 3) {
        flood[i] = 'a'; flood[i + 1] = '\r'; flood[i + 2] = '\n';
    }
    size_t end = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)flood, n, &end) == AXL_ERR,
               "ssh ident: unbounded preamble is refused");

    /* Control: the same shape, but short enough to stay under the cap, must
       still be scanned rather than rejected -- otherwise the test above would
       pass against a parser that refuses ALL preamble. */
    size_t small = 300;
    test_check(axl_ssh_parse_ident((const uint8_t *)flood, small, &end) == AXL_INCOMPLETE,
               "ssh ident: preamble under the cap is still scanned");
    axl_free(flood);
}

static void
test_ssh_packet_frame(void)
{
    AxlString *b = axl_string_new(NULL);
    const char *msg = "\x05test";              /* SSH_MSG_SERVICE_REQUEST + body */
    test_check(axl_ssh_packet_wrap(b, msg, 5) == AXL_OK, "ssh packet: wrap ok");

    size_t total = axl_string_len(b);
    test_check(total % 8 == 0, "ssh packet: total is a multiple of 8");
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    test_check(d[4] >= 4, "ssh packet: at least 4 bytes of padding");

    size_t consumed = 0; const uint8_t *pl = NULL; uint32_t pn = 0;
    test_check(axl_ssh_packet_unwrap(d, total, &consumed, &pl, &pn) == AXL_OK,
               "ssh packet: unwrap ok");
    test_check(pn == 5, "ssh packet: payload length");
    test_check(axl_memcmp(pl, msg, 5) == 0, "ssh packet: payload bytes");
    test_check(consumed == total, "ssh packet: consumed the whole frame");
    axl_string_free(b);
}

static void
test_ssh_packet_partial_and_hostile(void)
{
    AxlString *b = axl_string_new(NULL);
    test_check(axl_ssh_packet_wrap(b, "\x05", 1) == AXL_OK, "ssh packet: wrap 1-byte ok");
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    size_t total = axl_string_len(b), consumed = 0;
    const uint8_t *pl = NULL; uint32_t pn = 0;

    test_check(axl_ssh_packet_unwrap(d, total - 1, &consumed, &pl, &pn) == AXL_INCOMPLETE,
               "ssh packet: short read is INCOMPLETE");

    /* A packet_length beyond our cap must be refused, not allocated.
       packet_length 40004 is deliberately BLOCK-ALIGNED (total 40008) and its
       padding is legal, so the size cap is the only rule that can reject it.
       The obvious vector (an arbitrary huge length) is rejected by the
       block-multiple rule first and leaves the cap untested -- which is what
       sabotage caught. */
    const uint8_t huge[9] = { 0x00, 0x00, 0x9C, 0x44, 0x04, 0, 0, 0, 0 };
    size_t c2 = 0;
    test_check(axl_ssh_packet_unwrap(huge, sizeof huge, &c2, &pl, &pn) == AXL_ERR,
               "ssh packet: oversized packet_length refused");

    /* padding_length larger than the packet is the other classic. Again
       block-aligned (packet_length 12, total 16) with padding above the
       minimum, so only the padding-vs-packet rule can reject it. */
    const uint8_t badpad[16] = { 0x00, 0x00, 0x00, 0x0C, 0xFF,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    size_t c3 = 0;
    test_check(axl_ssh_packet_unwrap(badpad, sizeof badpad, &c3, &pl, &pn) == AXL_ERR,
               "ssh packet: padding_length past the packet refused");
    axl_string_free(b);
}

static void
test_ssh_packet_rfc_bounds(void)
{
    size_t c = 0; const uint8_t *pl = NULL; uint32_t pn = 0;

    /* CONTROL FIRST: a frame that satisfies every rule must be ACCEPTED, so the
       two rejections below cannot be passing against a parser that refuses
       everything. packet_length 12 => total 16 (a multiple of 8), padding 4
       (the minimum), payload 7. */
    const uint8_t good[16] = { 0x00, 0x00, 0x00, 0x0C, 0x04,
                               'p', 'a', 'y', 'l', 'o', 'a', 'd',
                               0, 0, 0, 0 };
    test_check(axl_ssh_packet_unwrap(good, sizeof good, &c, &pl, &pn) == AXL_OK,
               "ssh packet: a fully conformant frame is accepted");
    test_check(pn == 7 && axl_memcmp(pl, "payload", 7) == 0,
               "ssh packet: conformant frame yields its payload");

    /* RFC 4253 section 6: "There MUST be at least four bytes of padding."
       Three is short. This frame is otherwise perfect -- total 16, a multiple
       of 8 -- so only the padding minimum can reject it. */
    const uint8_t shortpad[16] = { 0x00, 0x00, 0x00, 0x0C, 0x03,
                                   'p', 'a', 'y', 'l', 'o', 'a', 'd', '!',
                                   0, 0, 0 };
    size_t c2 = 0;
    test_check(axl_ssh_packet_unwrap(shortpad, sizeof shortpad, &c2, &pl, &pn) == AXL_ERR,
               "ssh packet: fewer than 4 padding bytes refused");

    /* RFC 4253 section 6: the total must be a multiple of the cipher block
       size (8 before NEWKEYS). packet_length 5 gives a total of 9. Padding is
       4, so ONLY the block-multiple rule can reject this one. */
    const uint8_t unaligned[9] = { 0x00, 0x00, 0x00, 0x05, 0x04, 0, 0, 0, 0 };
    size_t c3 = 0;
    test_check(axl_ssh_packet_unwrap(unaligned, sizeof unaligned, &c3, &pl, &pn) == AXL_ERR,
               "ssh packet: total not a multiple of 8 refused");

    /* A truncated length prefix cannot be distinguished from a slow peer. */
    size_t c4 = 0;
    test_check(axl_ssh_packet_unwrap(good, 3, &c4, &pl, &pn) == AXL_INCOMPLETE,
               "ssh packet: fewer than 4 bytes is INCOMPLETE");

    size_t c5 = 0;
    test_check(axl_ssh_packet_unwrap(NULL, 16, &c5, &pl, &pn) == AXL_ERR,
               "ssh packet: NULL buf errs");
    test_check(axl_ssh_packet_unwrap(good, sizeof good, NULL, &pl, &pn) == AXL_ERR,
               "ssh packet: NULL consumed errs");
    test_check(axl_ssh_packet_unwrap(good, sizeof good, &c5, NULL, &pn) == AXL_ERR,
               "ssh packet: NULL payload errs");
    test_check(axl_ssh_packet_unwrap(good, sizeof good, &c5, &pl, NULL) == AXL_ERR,
               "ssh packet: NULL payload_len errs");
}

/* Build a KEXINIT payload from ten name-lists, so each test varies exactly one
   slot instead of re-spelling the whole message. */
static AxlString *
ssh_build_kexinit(const char *const lists[10])
{
    AxlString *b = axl_string_new(NULL);
    char hdr = (char)AXL_SSH_MSG_KEXINIT;
    char cookie[16] = { 0 };
    char tail[5] = { 0 };            /* first_kex_packet_follows + reserved */
    axl_string_append_len(b, &hdr, 1);
    axl_string_append_len(b, cookie, sizeof cookie);
    for (int i = 0; i < 10; i++) {
        axl_ssh_put_string(b, lists[i], axl_strlen(lists[i]));
    }
    axl_string_append_len(b, tail, sizeof tail);
    return b;
}

/* Every slot filled with something we accept. Tests copy this and spoil one. */
static const char *const ssh_kexinit_ok[10] = {
    "curve25519-sha256", "ssh-ed25519",
    "chacha20-poly1305@openssh.com", "chacha20-poly1305@openssh.com",
    "", "", "none", "none", "", ""
};

static void
test_ssh_kexinit_build(void)
{
    AxlString *b = axl_string_new(NULL);
    test_check(axl_ssh_kexinit_build(b, true) == AXL_OK, "ssh kexinit: build ok");
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    test_check(d[0] == AXL_SSH_MSG_KEXINIT, "ssh kexinit: message number 20");
    test_check(axl_string_len(b) > 1 + 16, "ssh kexinit: carries a 16-byte cookie");

    /* Our own KEXINIT must satisfy our own selector — the round trip. */
    test_check(axl_ssh_kexinit_select(d, axl_string_len(b), NULL) == AXL_OK,
               "ssh kexinit: our own offer is selectable");
    axl_string_free(b);
}

static void
test_ssh_kexinit_rejects_unsupported(void)
{
    /* CONTROL: the reference offer must be ACCEPTED, so every rejection below
       is about the one slot it spoils and not about the harness. */
    AxlString *ok = ssh_build_kexinit(ssh_kexinit_ok);
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(ok),
                                      axl_string_len(ok), NULL) == AXL_OK,
               "ssh kexinit: reference offer accepted");
    axl_string_free(ok);

    /* A peer offering only algorithms we refuse must be rejected, not
       negotiated down. One slot spoiled per case. */
    static const struct { int slot; const char *value; const char *label; } bad[] = {
        { 0, "diffie-hellman-group1-sha1", "unsupported kex refused, not downgraded" },
        { 1, "ssh-rsa",                    "unsupported host key refused" },
        { 2, "3des-cbc",                   "unsupported c2s cipher refused" },
        { 3, "3des-cbc",                   "unsupported s2c cipher refused" },
        { 6, "zlib",                       "compression-only c2s peer refused" },
        { 7, "zlib",                       "compression-only s2c peer refused" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *lists[10];
        for (int j = 0; j < 10; j++) {
            lists[j] = ssh_kexinit_ok[j];
        }
        lists[bad[i].slot] = bad[i].value;
        AxlString *b = ssh_build_kexinit(lists);
        test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(b),
                                          axl_string_len(b), NULL) == AXL_ERR,
                   bad[i].label);
        axl_string_free(b);
    }

    /* A list is a SET: our name alongside others we do not want must still be
       accepted, which is what makes the refusals above meaningful rather than
       "we only accept a list of exactly one name". */
    const char *mixed[10];
    for (int j = 0; j < 10; j++) {
        mixed[j] = ssh_kexinit_ok[j];
    }
    mixed[0] = "diffie-hellman-group14-sha1,curve25519-sha256,ext-info-c";
    mixed[6] = "none,zlib@openssh.com";
    mixed[7] = "none,zlib@openssh.com";
    AxlString *m = ssh_build_kexinit(mixed);
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(m),
                                      axl_string_len(m), NULL) == AXL_OK,
               "ssh kexinit: our name among others is accepted");
    axl_string_free(m);
}

static void
test_ssh_kexinit_structure(void)
{
    AxlString *b = ssh_build_kexinit(ssh_kexinit_ok);
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    size_t full = axl_string_len(b);

    /* A KEXINIT that stops after the slots we happen to inspect must still be
       refused: a peer that sends four lists and nothing else is malformed, and
       accepting it would mean our parser only validates what it reads. */
    test_check(axl_ssh_kexinit_select(d, full - 5, NULL) == AXL_ERR,
               "ssh kexinit: missing trailing fields refused");
    test_check(axl_ssh_kexinit_select(d, 20, NULL) == AXL_ERR,
               "ssh kexinit: truncated mid-list refused");
    test_check(axl_ssh_kexinit_select(d, 16, NULL) == AXL_ERR,
               "ssh kexinit: shorter than the cookie refused");

    /* Wrong message number, right shape. */
    AxlString *w = ssh_build_kexinit(ssh_kexinit_ok);
    ((char *)axl_string_data(w))[0] = 21;   /* SSH_MSG_NEWKEYS */
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(w),
                                      axl_string_len(w), NULL) == AXL_ERR,
               "ssh kexinit: wrong message number refused");
    axl_string_free(w);

    test_check(axl_ssh_kexinit_select(NULL, full, NULL) == AXL_ERR,
               "ssh kexinit: NULL buf errs");
    axl_string_free(b);
}

static void
test_ssh_kdf_chains(void)
{
    /* KNOWN ANSWER, not merely "fixed inputs". The whole risk this guards is
       a derivation that is self-consistent and wrong: two copies of our own
       bug interoperate happily and OpenSSH does not. Determinism cannot catch
       that -- a swapped field order is perfectly deterministic. So the bytes
       below were computed independently (python hashlib) from
       K1 = SHA256(K || H || letter || session_id),
       K2 = SHA256(K || H || K1), and are pinned exactly. */
    const uint8_t k[4] = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t h[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

    static const uint8_t want_a1[32] = {
        0xB8, 0x33, 0xDB, 0xA0, 0x01, 0x7F, 0x7B, 0x1E,
        0x8B, 0xA8, 0x01, 0x73, 0x27, 0x8C, 0x45, 0xDB,
        0xC1, 0x7C, 0x8E, 0xB7, 0x57, 0x7E, 0x60, 0x9F,
        0x19, 0xF2, 0x03, 0x25, 0xB9, 0x5E, 0x57, 0x50
    };
    static const uint8_t want_b1[32] = {
        0x8F, 0x6C, 0xAE, 0x9A, 0xE2, 0x21, 0xE6, 0x63,
        0x43, 0xE4, 0xE7, 0xF5, 0xE7, 0x8B, 0x1A, 0x22,
        0x8C, 0xD1, 0xB4, 0xC2, 0x26, 0x7F, 0xA4, 0x1C,
        0xB8, 0xFD, 0x83, 0x38, 0x3D, 0xB3, 0xDC, 0x60
    };
    static const uint8_t want_a2[32] = {
        0x3B, 0x9D, 0x7B, 0x0C, 0xD1, 0x51, 0x2D, 0x0D,
        0x79, 0x53, 0xEB, 0x75, 0xA5, 0xB0, 0xE1, 0x8C,
        0x0A, 0x47, 0x97, 0x15, 0x37, 0x45, 0x8A, 0x19,
        0xAF, 0xDA, 0x19, 0xD1, 0x7D, 0xF8, 0xEE, 0x5E
    };

    uint8_t out1[32] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           out1, sizeof out1) == AXL_OK, "ssh kdf: ok");
    test_check(axl_memcmp(out1, want_a1, sizeof want_a1) == 0,
               "ssh kdf: K1 for 'A' matches the independently computed digest");

    uint8_t outB[32] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'B', h, sizeof h,
                           outB, sizeof outB) == AXL_OK, "ssh kdf: letter B ok");
    test_check(axl_memcmp(outB, want_b1, sizeof want_b1) == 0,
               "ssh kdf: K1 for 'B' matches the independently computed digest");
    /* The letter must change the output -- otherwise IV and key are identical,
       which is catastrophic and easy to do. */
    test_check(axl_memcmp(out1, outB, sizeof out1) != 0,
               "ssh kdf: a different letter gives different bytes");

    /* Longer than one hash block must CHAIN, not repeat the first block. */
    uint8_t big[64] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           big, sizeof big) == AXL_OK, "ssh kdf: 64-byte output ok");
    test_check(axl_memcmp(big, want_a1, 32) == 0,
               "ssh kdf: chained first block still matches K1");
    test_check(axl_memcmp(big + 32, want_a2, 32) == 0,
               "ssh kdf: second block matches the independently computed K2");
    test_check(axl_memcmp(big, big + 32, 32) != 0,
               "ssh kdf: second block differs from the first");

    /* THREE blocks, because two cannot tell the chaining apart. Kn+1 hashes
       the WHOLE output so far (K1 || ... || Kn), and at the second block
       "everything so far" and "the previous block" are the same 32 bytes -- so
       a 64-byte vector passes against both. Only K3 separates them. */
    static const uint8_t want_a3[32] = {
        0x68, 0xA0, 0xAA, 0x05, 0xBD, 0x58, 0x83, 0xFF,
        0xB3, 0xF0, 0x63, 0x81, 0x22, 0xAC, 0xB2, 0xF7,
        0xDD, 0x76, 0xD2, 0x9E, 0xBA, 0x4B, 0x37, 0x21,
        0x02, 0xBA, 0x43, 0x60, 0x54, 0x5D, 0x22, 0x9E
    };
    uint8_t big3[96] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           big3, sizeof big3) == AXL_OK, "ssh kdf: 96-byte output ok");
    test_check(axl_memcmp(big3 + 64, want_a3, 32) == 0,
               "ssh kdf: K3 hashes the whole output so far, not just K2");

    /* A length that is not a whole number of blocks must truncate the last
       block, not overrun the caller's buffer. */
    uint8_t odd[40] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           odd, sizeof odd) == AXL_OK, "ssh kdf: 40-byte output ok");
    test_check(axl_memcmp(odd, want_a1, 32) == 0 &&
               axl_memcmp(odd + 32, want_a2, 8) == 0,
               "ssh kdf: partial final block is the truncated next block");

    test_check(axl_ssh_kdf(NULL, 4, h, sizeof h, 'A', h, sizeof h, out1, sizeof out1)
               == AXL_ERR, "ssh kdf: NULL K errs");
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h, NULL, 32)
               == AXL_ERR, "ssh kdf: NULL out errs");
}

// ---------------------------------------------------------------------------
// mpint (RFC 4251 section 5)
// ---------------------------------------------------------------------------

/* Encode `in` as an mpint and compare the whole field, in hex, against
   `want`. One comparison rather than a length check plus a memcmp, so a
   failure names the vector. */
static void
check_mpint(const char *in_hex, const char *want, const char *name)
{
    uint8_t in[16];
    size_t  n = axl_strlen(in_hex) / 2;
    uint8_t got[32];
    if (n > sizeof in || !test_hex_bytes(in_hex, in, n)) {
        test_check(false, name);
        return;
    }
    AxlString *b = axl_string_new("");
    if (b == NULL) {
        test_check(false, name);
        return;
    }
    bool ok = axl_ssh_put_mpint(b, in, n) == AXL_OK
              && axl_string_len(b) <= sizeof got;
    if (ok) {
        axl_memcpy(got, axl_string_data(b), axl_string_len(b));
        ok = test_hex_equals(got, axl_string_len(b), want);
    }
    test_check(ok, name);
    axl_string_free(b);
}

static void
test_ssh_mpint(void)
{
    /* RFC 4251 section 5's own examples, restricted to the unsigned
       magnitudes this encoder takes. */
    check_mpint("00000000", "00000000",
                "ssh mpint: zero is a zero-length string");
    check_mpint("09a378f9b2e332a7", "0000000809a378f9b2e332a7",
                "ssh mpint: RFC 4251 example 09a3..a7");
    /* The case the whole function exists for: a top bit that would read as
       a negative number gets a 0x00 in front. */
    check_mpint("80", "000000020080",
                "ssh mpint: 0x80 gains a leading zero byte");
    check_mpint("0000000042", "0000000142",
                "ssh mpint: leading zero bytes are stripped");
    check_mpint("ff", "0000000200ff",
                "ssh mpint: 0xff gains a leading zero byte");
    check_mpint("7f", "000000017f",
                "ssh mpint: 0x7f does not");

    AxlString *b = axl_string_new("");
    test_check(b != NULL && axl_ssh_put_mpint(NULL, (const uint8_t *)"", 0)
                                == AXL_ERR,
               "ssh mpint: NULL builder refused");
    test_check(b != NULL && axl_ssh_put_mpint(b, NULL, 1) == AXL_ERR,
               "ssh mpint: NULL value with a non-zero length refused");
    axl_string_free(b);
}

// ---------------------------------------------------------------------------
// Host key and signature blobs
// ---------------------------------------------------------------------------

static void
test_ssh_blobs(void)
{
    uint8_t pub[32];
    for (size_t i = 0; i < sizeof pub; i++) {
        pub[i] = (uint8_t)i;
    }
    AxlString *b = axl_string_new("");
    test_check(b != NULL && axl_ssh_hostkey_blob(b, pub) == AXL_OK,
               "ssh blob: hostkey builds");
    if (b != NULL) {
        /* string("ssh-ed25519") || string(pub32). Pinned whole: this blob is
           hashed into H and sent on the wire, so a length or ordering slip
           is a value ssh(1) rejects and our own code would not notice. */
        uint8_t got[64];
        bool fits = axl_string_len(b) <= sizeof got;
        if (fits) {
            axl_memcpy(got, axl_string_data(b), axl_string_len(b));
        }
        test_check(fits && axl_string_len(b) == 51
                       && test_hex_equals(got, axl_string_len(b),
                              "0000000b7373682d65643235353139"
                              "00000020000102030405060708090a0b0c0d0e0f"
                              "101112131415161718191a1b1c1d1e1f"),
                   "ssh blob: hostkey is string(name) || string(pub)");
    }
    axl_string_free(b);

    uint8_t sig[64];
    for (size_t i = 0; i < sizeof sig; i++) {
        sig[i] = (uint8_t)(0x40 + i);
    }
    AxlString *sb = axl_string_new("");
    test_check(sb != NULL && axl_ssh_signature_blob(sb, sig) == AXL_OK
                   && axl_string_len(sb) == 83,
               "ssh blob: signature is 4+11 + 4+64 bytes");
    axl_string_free(sb);

    test_check(axl_ssh_hostkey_blob(NULL, pub) == AXL_ERR
                   && axl_ssh_signature_blob(NULL, sig) == AXL_ERR,
               "ssh blob: NULL builder refused");
}

// ---------------------------------------------------------------------------
// Exchange hash
// ---------------------------------------------------------------------------

static void
test_ssh_exchange_hash(void)
{
    /* Computed independently, in Python, from RFC 4253 section 8's formula:
         H = SHA256(string(V_C) || string(V_S) || string(I_C) || string(I_S)
                    || string(K_S) || string(Q_C) || string(Q_S) || mpint(K))
       Every one of those eight fields is a place to be self-consistently
       wrong, so this pins the bytes rather than the shape. */
    static const char *V_C = "SSH-2.0-OpenSSH_9.9";
    static const char *V_S = "SSH-2.0-AxlSsh_1.0";
    uint8_t i_c[25], i_s[25], k_s[51], q_c[32], q_s[32], k[32];
    bool ok =
        test_hex_bytes("14000102030405060708090a0b0c0d0e0f0000000474657374",
                       i_c, sizeof i_c)
        && test_hex_bytes("14101112131415161718191a1b1c1d1e1f0000000461786c73",
                          i_s, sizeof i_s)
        && test_hex_bytes("0000000b7373682d6564323535313900000020"
                          "000102030405060708090a0b0c0d0e0f"
                          "101112131415161718191a1b1c1d1e1f", k_s, sizeof k_s)
        && test_hex_bytes("202122232425262728292a2b2c2d2e2f"
                          "303132333435363738393a3b3c3d3e3f", q_c, sizeof q_c)
        && test_hex_bytes("404142434445464748494a4b4c4d4e4f"
                          "505152535455565758595a5b5c5d5e5f", q_s, sizeof q_s)
        && test_hex_bytes("606162636465666768696a6b6c6d6e6f"
                          "707172737475767778797a7b7c7d7e7f", k, sizeof k);
    test_check(ok, "ssh exchange hash: the vectors are well-formed");
    if (!ok) {
        return;
    }

    uint8_t h[AXL_SSH_HASH_LEN];
    test_check(axl_ssh_exchange_hash(h, sizeof h, V_C, V_S,
                                     i_c, sizeof i_c, i_s, sizeof i_s,
                                     k_s, sizeof k_s, q_c, sizeof q_c,
                                     q_s, sizeof q_s, k, sizeof k) == AXL_OK,
               "ssh exchange hash: computes");
    test_check(test_hex_equals(h, sizeof h,
                   "a458455840341784adba874eaa946632"
                   "c12ca7857173f4925e4c3725d450911e"),
               "ssh exchange hash: matches the independently computed H");

    test_check(axl_ssh_exchange_hash(h, sizeof h, NULL, V_S,
                                     i_c, sizeof i_c, i_s, sizeof i_s,
                                     k_s, sizeof k_s, q_c, sizeof q_c,
                                     q_s, sizeof q_s, k, sizeof k) == AXL_ERR,
               "ssh exchange hash: a NULL field is refused");
    test_check(axl_ssh_exchange_hash(h, 16, V_C, V_S,
                                     i_c, sizeof i_c, i_s, sizeof i_s,
                                     k_s, sizeof k_s, q_c, sizeof q_c,
                                     q_s, sizeof q_s, k, sizeof k) == AXL_ERR,
               "ssh exchange hash: a short output buffer is refused");
}

// ---------------------------------------------------------------------------
// chacha20-poly1305@openssh.com
// ---------------------------------------------------------------------------

static void
test_ssh_aead(void)
{
    uint8_t key[AXL_SSH_AEAD_KEY_LEN];
    for (size_t i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)i;
    }
    AxlSshAead a;
    test_check(axl_ssh_aead_set_key(&a, key) == AXL_OK,
               "ssh aead: key loads");
    test_check(axl_ssh_aead_set_key(NULL, key) == AXL_ERR
                   && axl_ssh_aead_set_key(&a, NULL) == AXL_ERR,
               "ssh aead: NULL arguments refused");

    /* THE INTEROP VECTOR. This packet was produced by an independent Python
       implementation of PROTOCOL.chacha20poly1305 -- two ChaCha20 keys, the
       length field under K_1 at counter 0, the payload under K_2 at counter
       1, and the Poly1305 key from K_2's counter-0 block. Decrypting it is
       the only assertion here that says anything about talking to OpenSSH; a
       seal/open round-trip is self-consistent by construction and would pass
       with the two keys swapped. */
    static const char *PACKET =
        "fb1a929286401cbb3246d702e0628979de95f73613b44cc7"
        "c0dd5f471c1aa53b9d6289d8dda7d6ff834f4842";
    uint8_t pkt[44];
    test_check(test_hex_bytes(PACKET, pkt, sizeof pkt),
               "ssh aead: the interop vector is well-formed");

    uint8_t  plain[64];
    size_t   consumed = 0;
    uint32_t plen = 0;
    test_check(axl_ssh_aead_open(&a, 3, pkt, sizeof pkt, &consumed,
                                 plain, sizeof plain, &plen) == AXL_OK
                   && consumed == sizeof pkt,
               "ssh aead: opens the independently produced packet");
    test_check(plen == 17
                   && test_hex_equals(plain, plen,
                                      "050000000c7373682d7573657261757468"),
               "ssh aead: recovers SERVICE_REQUEST(ssh-userauth) exactly");

    /* The sequence number IS the nonce, so the same bytes under a different
       seq must not authenticate. */
    test_check(axl_ssh_aead_open(&a, 4, pkt, sizeof pkt, &consumed,
                                 plain, sizeof plain, &plen) == AXL_ERR,
               "ssh aead: the wrong sequence number fails the tag");

    /* One flipped bit anywhere must fail the tag, not decrypt to garbage. */
    uint8_t bad[44];
    axl_memcpy(bad, pkt, sizeof bad);
    bad[20] ^= 0x01;
    test_check(axl_ssh_aead_open(&a, 3, bad, sizeof bad, &consumed,
                                 plain, sizeof plain, &plen) == AXL_ERR,
               "ssh aead: a flipped ciphertext bit fails the tag");
    axl_memcpy(bad, pkt, sizeof bad);
    bad[sizeof bad - 1] ^= 0x80;
    test_check(axl_ssh_aead_open(&a, 3, bad, sizeof bad, &consumed,
                                 plain, sizeof plain, &plen) == AXL_ERR,
               "ssh aead: a flipped tag bit fails");

    /* A short buffer is INCOMPLETE, not an error -- the caller is a stream
       reader and must be able to wait for more bytes. Every truncation
       point, because the length field is decrypted before the tag is
       checked and the boundary between "wait" and "reject" is where a
       parser desync would live. */
    bool all_incomplete = true;
    for (size_t n = 0; n < sizeof pkt; n++) {
        if (axl_ssh_aead_open(&a, 3, pkt, n, &consumed, plain, sizeof plain,
                              &plen) != AXL_INCOMPLETE) {
            all_incomplete = false;
        }
    }
    test_check(all_incomplete,
               "ssh aead: every truncation is INCOMPLETE, never ERR or OK");

    /* Round-trip, which proves seal and open agree even though only open is
       pinned against an outside implementation. */
    static const uint8_t msg[9] = { 21, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    AxlSshRngPool pad_pool;
    axl_ssh_rng_pool_init(&pad_pool);
    AxlString *out = axl_string_new("");
    test_check(out != NULL
                   && axl_ssh_aead_seal(&a, 7, msg, sizeof msg, &pad_pool, out)
                          == AXL_OK,
               "ssh aead: seals");
    if (out != NULL) {
        const uint8_t *sealed = (const uint8_t *)axl_string_data(out);
        size_t         slen   = axl_string_len(out);
        test_check(axl_ssh_aead_open(&a, 7, sealed, slen, &consumed, plain,
                                     sizeof plain, &plen) == AXL_OK
                       && consumed == slen && plen == sizeof msg
                       && axl_memcmp(plain, msg, sizeof msg) == 0,
                   "ssh aead: seal/open round-trips the payload");
        /* And the same packet under a different seq must not open, which is
           what proves seal actually used the sequence number. */
        test_check(axl_ssh_aead_open(&a, 8, sealed, slen, &consumed, plain,
                                     sizeof plain, &plen) == AXL_ERR,
                   "ssh aead: a sealed packet is bound to its sequence number");
    }
    axl_string_free(out);

    /* P4b: sealing draws padding from a per-connection pool rather than one
       firmware call per packet. 20 small packets (6 bytes of padding each
       for this 9-byte message, 120 bytes total) must cost exactly one
       refill from a fresh pool -- proving the pool, not axl_rng_bytes
       directly, is what fed the seal above. */
    AxlSshRngPool pool20;
    axl_ssh_rng_pool_init(&pool20);
    unsigned pool_seal_failures = 0;
    for (unsigned i = 0; i < 20; i++) {
        AxlString *small = axl_string_new("");
        if (small == NULL
            || axl_ssh_aead_seal(&a, 100 + i, msg, sizeof msg, &pool20, small)
                   != AXL_OK) {
            pool_seal_failures++;
        }
        axl_string_free(small);
    }
    test_check(pool_seal_failures == 0 && pool20.refills == 1,
               "ssh aead: 20 small seals cost exactly one pool refill");

    /* A pool refill FAILURE (axl_rng_bytes returning an error) has no test
       seam to reach: axl_rng_bytes calls EFI_RNG_PROTOCOL directly
       (src/util/axl-rng.c's get_rng(), the protocol pointer cached from
       LocateProtocol) with no fault-injection hook, and a production seam
       is not worth adding for one test branch. Verified by review instead:
       axl_ssh_rng_pool_take returns AXL_ERR the instant axl_rng_bytes
       fails, leaves the pool's pos at AXL_SSH_RNG_POOL_LEN (empty, never
       zero-filled), and never touches its `out` argument; axl_ssh_aead_seal's
       call to it takes the same `goto out` path on that AXL_ERR that a
       direct axl_rng_bytes failure took before this change, which scrubs
       `clear` and `buf` and returns AXL_ERR without ever reaching
       axl_string_append_len. So a refill failure fails the seal and emits
       nothing, exactly as before.

       That is the FAILURE case. The CONTENT case -- a refill that reports
       success but hands out predictable bytes -- needs no production seam
       either, and IS covered, by the tests below and in test_ssh_rng_pool:
       a freshly refilled pool is not all-zero (with "clear scrubs every
       byte" right after it as that same buffer's control, so the check
       cannot pass vacuously on a pool that never really refills), two
       consecutive takes from one fill differ, and two refills of one pool
       produce different content. The differential check right below this
       comment catches a seal that ignores what the pool actually returned.
       Together these are what a "hand out zeros without refilling"
       sabotage now fails, where before this round it failed nothing. */
    AxlSshRngPool diff_pool;
    axl_ssh_rng_pool_init(&diff_pool);
    AxlString *seal_a = axl_string_new("");
    AxlString *seal_b = axl_string_new("");
    /* Reusing seq 200 for both calls is deliberate and ONLY safe here: this
       is a byte-comparison fixture, never a transport. Production code must
       never do this -- axl_ssh_aead_seal's own docstring says a caller that
       fails to advance seq reuses a keystream -- but a fixed nonce is
       exactly what isolates padding as the only thing that can differ:
       same key and seq give the same keystream for the length field and the
       payload, so if the ciphertexts differ at all, the pool fed different
       padding bytes into the two calls. */
    test_check(seal_a != NULL && seal_b != NULL
                   && axl_ssh_aead_seal(&a, 200, msg, sizeof msg, &diff_pool,
                                        seal_a) == AXL_OK
                   && axl_ssh_aead_seal(&a, 200, msg, sizeof msg, &diff_pool,
                                        seal_b) == AXL_OK,
               "ssh aead: two same-seq seals for the differential check");
    if (seal_a != NULL && seal_b != NULL) {
        size_t la = axl_string_len(seal_a);
        size_t lb = axl_string_len(seal_b);
        test_check(la == lb
                       && axl_memcmp(axl_string_data(seal_a),
                                     axl_string_data(seal_b), la) != 0,
                   "ssh aead: two same-key/same-seq seals differ, which only"
                   " the pool's padding can explain"
                   " (false-failure probability 2^-48)");
    }
    axl_string_free(seal_a);
    axl_string_free(seal_b);

    /* An output buffer too small for the payload must be refused, not
       truncated into. */
    test_check(axl_ssh_aead_open(&a, 3, pkt, sizeof pkt, &consumed, plain, 4,
                                 &plen) == AXL_ERR,
               "ssh aead: a too-small output buffer is refused");
}

/* P4b: the padding pool refills with ONE firmware call per block. */
static void
test_ssh_rng_pool(void)
{
    AxlSshRngPool p;
    uint8_t out[AXL_SSH_RNG_POOL_LEN];
    bool any_nonzero = false;

    axl_ssh_rng_pool_init(&p);
    test_check(p.refills == 0 && p.pos == AXL_SSH_RNG_POOL_LEN,
               "rng pool: starts empty, no firmware call yet");
    test_check(axl_ssh_rng_pool_take(&p, out, 8) == AXL_OK && p.refills == 1,
               "rng pool: first take refills once");
    unsigned quiet_failures = 0;
    for (unsigned i = 1; i < AXL_SSH_RNG_POOL_LEN / 8; i++) {
        if (axl_ssh_rng_pool_take(&p, out, 8) != AXL_OK) {
            quiet_failures++;
        }
    }
    test_check(quiet_failures == 0,
               "rng pool: every 8-byte take up to the block boundary succeeds");
    test_check(p.refills == 1 && p.pos == AXL_SSH_RNG_POOL_LEN,
               "rng pool: 256 bytes in 8-byte takes cost exactly one refill");
    test_check(axl_ssh_rng_pool_take(&p, out, 8) == AXL_OK && p.refills == 2,
               "rng pool: the 257th byte refills again");
    /* 11 bytes with 3 left: discard the tail, refill, take 11. */
    p.pos = AXL_SSH_RNG_POOL_LEN - 3;
    test_check(axl_ssh_rng_pool_take(&p, out, 11) == AXL_OK && p.refills == 3
                   && p.pos == 11,
               "rng pool: a take larger than the remainder refills first");
    test_check(axl_ssh_rng_pool_take(&p, out, AXL_SSH_RNG_POOL_LEN + 1)
                   == AXL_ERR,
               "rng pool: a take over the pool size is refused");
    test_check(axl_ssh_rng_pool_take(NULL, out, 4) == AXL_ERR,
               "rng pool: NULL pool is refused");

    /* Content checks (P4b review fix): the tests above only ever look at
       `pos`/`refills` bookkeeping, which a refill that reports success
       without ever writing the buffer leaves perfectly intact -- that
       defect showed up UNDETECTED under sabotage until these were added.
       p.pos == 11 here (set by the "take larger than the remainder"
       check above, with plenty of the same fill still unread), so neither
       of these next two takes triggers its own refill -- both read the
       SAME fill, differing only by the offset `pos` should have advanced
       between them. */
    uint8_t block_a[8], block_b[8];
    test_check(axl_ssh_rng_pool_take(&p, block_a, 8) == AXL_OK
                   && axl_ssh_rng_pool_take(&p, block_b, 8) == AXL_OK
                   && axl_memcmp(block_a, block_b, sizeof block_a) != 0,
               "rng pool: two consecutive 8-byte takes differ"
               " (false-failure probability 2^-64)");

    /* Two refills of one pool must not produce the same bytes -- catches a
       refill that reports success without actually rewriting the buffer,
       even on a pool whose buffer was never all-zero to begin with (so a
       "still zero" check alone would not catch a SECOND no-op refill that
       leaves stale-but-nonzero bytes in place). */
    uint8_t refill_1[AXL_SSH_RNG_POOL_LEN], refill_2[AXL_SSH_RNG_POOL_LEN];
    uint8_t tmp8[8];
    axl_ssh_rng_pool_init(&p);
    test_check(axl_ssh_rng_pool_take(&p, tmp8, 8) == AXL_OK && p.refills == 1,
               "rng pool: first refill for the cross-refill content check");
    axl_memcpy(refill_1, p.buf, sizeof refill_1);
    p.pos = AXL_SSH_RNG_POOL_LEN; /* exhausted: the next take must refill */
    test_check(axl_ssh_rng_pool_take(&p, tmp8, 8) == AXL_OK && p.refills == 2,
               "rng pool: forcing pos = LEN triggers a second refill");
    axl_memcpy(refill_2, p.buf, sizeof refill_2);
    test_check(axl_memcmp(refill_1, refill_2, sizeof refill_1) != 0,
               "rng pool: two refills of one pool produce different content"
               " (false-failure probability 2^-2048)");

    /* Re-init fresh, then take the WHOLE pool in one call so `out` equals
       the entire freshly refilled `p.buf` exactly. This must run
       immediately before clear() below: it is that check's CONTROL. Without
       it, "clear scrubs every byte" could pass vacuously on a pool that
       never really refills -- axl_ssh_rng_pool_init's own zero-fill already
       satisfies "all zero" before clear ever runs, so a no-op refill would
       leave the buffer looking exactly as clear() is supposed to leave it. */
    uint8_t full[AXL_SSH_RNG_POOL_LEN];
    bool    full_nonzero = false;
    axl_ssh_rng_pool_init(&p);
    test_check(axl_ssh_rng_pool_take(&p, full, AXL_SSH_RNG_POOL_LEN) == AXL_OK
                   && axl_memcmp(full, p.buf, AXL_SSH_RNG_POOL_LEN) == 0,
               "rng pool: taking the whole pool copies exactly p.buf");
    for (unsigned i = 0; i < AXL_SSH_RNG_POOL_LEN; i++) {
        full_nonzero |= full[i] != 0;
    }
    test_check(full_nonzero,
               "rng pool: a freshly refilled pool is not all-zero"
               " (false-failure probability 2^-2048)");

    axl_ssh_rng_pool_clear(&p);
    for (unsigned i = 0; i < AXL_SSH_RNG_POOL_LEN; i++) {
        any_nonzero |= p.buf[i] != 0;
    }
    test_check(!any_nonzero && p.pos == AXL_SSH_RNG_POOL_LEN,
               "rng pool: clear scrubs every byte and empties it");
}

/* Frame i of the ordering test: its length and its bytes are both functions
   of i, so a frame swapped with a neighbour, dropped or repeated changes the
   concatenation, and no two adjacent frames are the same length or fill. */
#define TXQ_ORDER_FRAMES 100u

static size_t
test_txq_frame_len(
    unsigned i  ///< frame index
)
{
    return 1u + (i * 7u) % 53u;
}

static uint8_t
test_txq_frame_byte(
    unsigned i,  ///< frame index
    size_t   k   ///< byte offset within the frame
)
{
    return (uint8_t)(i * 31u + k * 3u + 1u);
}

static void
test_ssh_txq(void)
{
    AxlSshTxq q;
    uint8_t   f10[10], f20[20], f30[30], f5[5], f7[7];
    uint8_t   want[60];
    uint8_t  *buf    = NULL;
    size_t    len    = 0;
    unsigned  frames = 0;

    axl_memset(f10, 0x11, sizeof f10);
    axl_memset(f20, 0x22, sizeof f20);
    axl_memset(f30, 0x33, sizeof f30);
    axl_memset(f5, 0x55, sizeof f5);
    axl_memset(f7, 0x77, sizeof f7);

    if (axl_ssh_txq_init(&q) != AXL_OK) {
        test_check(false, "ssh txq: init");
        return;
    }
    test_check(!axl_ssh_txq_ready(&q) && axl_ssh_txq_bytes(&q) == 0,
               "ssh txq: a new queue is empty and not ready");
    test_check(axl_ssh_txq_take(&q, &buf, &len, &frames) == AXL_ERR
                   && buf == NULL && len == 0 && frames == 0,
               "ssh txq: take on an empty queue is refused, outputs untouched");
    test_check(axl_ssh_txq_push(&q, f10, 0) == AXL_ERR
                   && axl_ssh_txq_bytes(&q) == 0 && q.pending_frames == 0,
               "ssh txq: a zero-length push is refused and counts nothing");

    test_check(axl_ssh_txq_push(&q, f10, sizeof f10) == AXL_OK
                   && axl_ssh_txq_push(&q, f20, sizeof f20) == AXL_OK
                   && axl_ssh_txq_push(&q, f30, sizeof f30) == AXL_OK,
               "ssh txq: three frames push");
    test_check(axl_ssh_txq_ready(&q) && axl_ssh_txq_bytes(&q) == 60,
               "ssh txq: 10+20+30 pending -> ready, 60 bytes queued");

    axl_memcpy(want, f10, 10);
    axl_memcpy(want + 10, f20, 20);
    axl_memcpy(want + 30, f30, 30);
    test_check(axl_ssh_txq_take(&q, &buf, &len, &frames) == AXL_OK
                   && buf != NULL && len == 60 && frames == 3
                   && axl_memcmp(buf, want, 60) == 0,
               "ssh txq: take hands over 60 bytes, 3 frames, in push order");
    test_check(!axl_ssh_txq_ready(&q) && q.inflight_len == 60
                   && q.inflight_frames == 3 && q.pending_frames == 0,
               "ssh txq: while a send is in flight the queue is not ready");

    uint8_t *second = NULL;
    size_t   len2   = 0;
    unsigned fr2    = 0;
    test_check(axl_ssh_txq_push(&q, f5, sizeof f5) == AXL_OK
                   && axl_ssh_txq_push(&q, f7, sizeof f7) == AXL_OK
                   && !axl_ssh_txq_ready(&q)
                   && axl_ssh_txq_bytes(&q) == 72,
               "ssh txq: two more wait behind the send; 60 + 12 = 72 queued");
    test_check(axl_ssh_txq_take(&q, &second, &len2, &fr2) == AXL_ERR
                   && second == NULL,
               "ssh txq: a second take while one is in flight is refused");

    axl_ssh_txq_sent(&q);
    axl_free(buf);
    buf = NULL;
    test_check(axl_ssh_txq_ready(&q) && axl_ssh_txq_bytes(&q) == 12,
               "ssh txq: sent -> ready again with 12 bytes pending");
    test_check(axl_ssh_txq_take(&q, &second, &len2, &fr2) == AXL_OK
                   && len2 == 12 && fr2 == 2
                   && axl_memcmp(second, f5, 5) == 0
                   && axl_memcmp(second + 5, f7, 7) == 0,
               "ssh txq: the next take is exactly the two, in order");

    /* Freed with a run IN FLIGHT (second, the caller's) and one PENDING:
       the queue releases the pending run, the caller its own buffer. The
       leak gate at teardown is what grades "releases everything". */
    test_check(axl_ssh_txq_push(&q, f30, sizeof f30) == AXL_OK,
               "ssh txq: push while in flight, before free");
    axl_ssh_txq_free(&q);
    axl_free(second);
    axl_ssh_txq_free(NULL);

    /* ORDER across many frames of varying sizes, pushed in several runs
       with takes in between, reassembled and compared byte for byte. */
    size_t total = 0;
    for (unsigned i = 0; i < TXQ_ORDER_FRAMES; i++) {
        total += test_txq_frame_len(i);
    }
    uint8_t *expect = axl_malloc(total);
    uint8_t *got    = axl_malloc(total);
    if (expect == NULL || got == NULL || axl_ssh_txq_init(&q) != AXL_OK) {
        test_check(false, "ssh txq order: setup");
        axl_free(expect);
        axl_free(got);
        return;
    }
    size_t   off = 0, got_len = 0;
    unsigned got_frames = 0, push_failures = 0, take_failures = 0;
    for (unsigned i = 0; i < TXQ_ORDER_FRAMES; i++) {
        uint8_t frame[64];
        size_t  fl = test_txq_frame_len(i);
        for (size_t k = 0; k < fl; k++) {
            frame[k] = test_txq_frame_byte(i, k);
        }
        axl_memcpy(expect + off, frame, fl);
        off += fl;
        if (axl_ssh_txq_push(&q, frame, fl) != AXL_OK) {
            push_failures++;
        }
        /* Take after every 13th frame: runs of uneven length, and a push
           always lands while the previous run is still "in flight". */
        if (i % 13u == 12u || i == TXQ_ORDER_FRAMES - 1u) {
            if (q.inflight_len != 0) {
                axl_ssh_txq_sent(&q);
            }
            uint8_t *run = NULL;
            size_t   rl  = 0;
            unsigned rf  = 0;
            if (axl_ssh_txq_take(&q, &run, &rl, &rf) != AXL_OK
                || got_len + rl > total) {
                take_failures++;
            } else {
                axl_memcpy(got + got_len, run, rl);
                got_len    += rl;
                got_frames += rf;
            }
            axl_free(run);
        }
    }
    axl_ssh_txq_sent(&q);
    test_check(push_failures == 0 && take_failures == 0,
               "ssh txq order: every push and take succeeds");
    test_check(got_len == total && got_frames == TXQ_ORDER_FRAMES
                   && axl_memcmp(got, expect, total) == 0,
               "ssh txq order: 100 frames of varying size arrive byte-exact"
               " and in order");
    test_check(!axl_ssh_txq_ready(&q) && axl_ssh_txq_bytes(&q) == 0,
               "ssh txq order: drained and nothing in flight");
    axl_ssh_txq_free(&q);
    axl_free(expect);
    axl_free(got);
}

static void
test_ssh_kexinit_guess_flag(void)
{
    /* RFC 4253 section 7.1's first_kex_packet_follows. The flag sits after
       the ten name-lists; a selector that merely checks five bytes remain
       (which this one did) never reads it, and the peer's guessed packet is
       then dispatched as if it were a real message -- killing a connection
       the RFC says to carry on. ssh(1) always writes 0 there, so the
       conformance test cannot see this either way. */
    AxlString *b = axl_string_new("");
    if (b == NULL || axl_ssh_kexinit_build(b, true) != AXL_OK) {
        test_check(false, "ssh kex guess: build");
        axl_string_free(b);
        return;
    }
    uint8_t *d   = (uint8_t *)axl_string_data(b);
    size_t   len = axl_string_len(b);

    /* Our own KEXINIT names our kex first, so a guess would be OURS. */
    AxlSshKexinitPeer peer = { .guessed_wrong = true,
                               .strict_kex = false };
    test_check(axl_ssh_kexinit_select(d, len, &peer) == AXL_OK
                   && !peer.guessed_wrong,
               "ssh kex guess: flag clear -> no packet to drop");

    /* Set the flag. Our kex is still first, so the guess is CORRECT and
       nothing is dropped -- the distinction the RFC draws, and the one a
       naive "flag set means drop" would get backwards. */
    d[len - 5] = 1;
    peer.guessed_wrong = true;
    test_check(axl_ssh_kexinit_select(d, len, &peer) == AXL_OK
                   && !peer.guessed_wrong,
               "ssh kex guess: flag set but guess is ours -> nothing dropped");
    axl_string_free(b);

    /* Now a peer whose FIRST kex name is something else, with the flag set.
       Its guess is for that other method, so its next packet must go. */
    AxlString *o = axl_string_new("");
    if (o == NULL) {
        test_check(false, "ssh kex guess: alloc");
        return;
    }
    char hdr = (char)AXL_SSH_MSG_KEXINIT;
    char cookie[16] = { 0 };
    bool ok = axl_string_append_len(o, &hdr, 1) == AXL_OK
              && axl_string_append_len(o, cookie, sizeof cookie) == AXL_OK;
    /* Our name is present but NOT first -- so selection still succeeds and
       only the guess is wrong. That pairing is the point: a test where the
       peer offers nothing we take would fail on selection and prove
       nothing about the flag. */
    static const char *const lists[AXL_SSH_KEXINIT_LISTS] = {
        "sntrup761x25519-sha512,curve25519-sha256",
        "ssh-ed25519", "chacha20-poly1305@openssh.com",
        "chacha20-poly1305@openssh.com", "", "", "none", "none", "", ""
    };
    for (int i = 0; ok && i < AXL_SSH_KEXINIT_LISTS; i++) {
        ok = axl_ssh_put_string(o, lists[i], axl_strlen(lists[i])) == AXL_OK;
    }
    char tail[5] = { 1, 0, 0, 0, 0 };   /* first_kex_packet_follows = 1 */
    ok = ok && axl_string_append_len(o, tail, sizeof tail) == AXL_OK;
    test_check(ok, "ssh kex guess: built a peer KEXINIT that guesses wrong");

    peer.guessed_wrong = false;
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(o),
                                      axl_string_len(o), &peer) == AXL_OK
                   && peer.guessed_wrong,
               "ssh kex guess: a wrong guess with the flag set is reported");

    /* And with the flag cleared, the same message drops nothing. */
    uint8_t *od = (uint8_t *)axl_string_data(o);
    od[axl_string_len(o) - 5] = 0;
    peer.guessed_wrong = true;
    test_check(axl_ssh_kexinit_select(od, axl_string_len(o), &peer) == AXL_OK
                   && !peer.guessed_wrong,
               "ssh kex guess: same message, flag clear -> nothing dropped");

    /* A NULL out-param must be accepted -- the unit tests and any caller
       that does not care both pass one. */
    test_check(axl_ssh_kexinit_select(od, axl_string_len(o), NULL) == AXL_OK,
               "ssh kex guess: a NULL out-param is allowed");
    axl_string_free(o);

    /* RFC 4253 section 7.1 says the guess is wrong when the kex "AND/OR the
       host key algorithm" is not the peer's first choice. A peer whose kex
       guess is RIGHT but whose host key guess is WRONG has still guessed
       wrong -- checking only the kex slot misses it, and its guessed packet
       is then dispatched as a real message. */
    AxlString *hk = axl_string_new("");
    if (hk == NULL) {
        test_check(false, "ssh kex guess: alloc");
        return;
    }
    char hdr2 = (char)AXL_SSH_MSG_KEXINIT;
    char ck[16] = { 0 };
    bool ok2 = axl_string_append_len(hk, &hdr2, 1) == AXL_OK
               && axl_string_append_len(hk, ck, sizeof ck) == AXL_OK;
    static const char *const hk_lists[AXL_SSH_KEXINIT_LISTS] = {
        "curve25519-sha256",                      /* our kex, FIRST */
        "rsa-sha2-512,ssh-ed25519",               /* ours, but NOT first */
        "chacha20-poly1305@openssh.com", "chacha20-poly1305@openssh.com",
        "", "", "none", "none", "", ""
    };
    for (int i = 0; ok2 && i < AXL_SSH_KEXINIT_LISTS; i++) {
        ok2 = axl_ssh_put_string(hk, hk_lists[i],
                                 axl_strlen(hk_lists[i])) == AXL_OK;
    }
    char tail2[5] = { 1, 0, 0, 0, 0 };
    ok2 = ok2 && axl_string_append_len(hk, tail2, sizeof tail2) == AXL_OK;
    test_check(ok2, "ssh kex guess: built a right-kex wrong-hostkey KEXINIT");

    AxlSshKexinitPeer hk_peer = { .guessed_wrong = false,
                                  .strict_kex = false };
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(hk),
                                      axl_string_len(hk), &hk_peer)
                   == AXL_OK && hk_peer.guessed_wrong,
               "ssh kex guess: a wrong HOST KEY guess is reported too");
    axl_string_free(hk);

    /* And a name that merely has ours as a PREFIX is not ours: a peer whose
       first kex is curve25519-sha256@libssh.org has guessed a different
       method, and a prefix test without the separator check would miss it. */
    AxlString *px = axl_string_new("");
    if (px == NULL) {
        test_check(false, "ssh kex guess: alloc");
        return;
    }
    bool ok3 = axl_string_append_len(px, &hdr2, 1) == AXL_OK
               && axl_string_append_len(px, ck, sizeof ck) == AXL_OK;
    static const char *const px_lists[AXL_SSH_KEXINIT_LISTS] = {
        "curve25519-sha256@libssh.org,curve25519-sha256",
        "ssh-ed25519", "chacha20-poly1305@openssh.com",
        "chacha20-poly1305@openssh.com", "", "", "none", "none", "", ""
    };
    for (int i = 0; ok3 && i < AXL_SSH_KEXINIT_LISTS; i++) {
        ok3 = axl_ssh_put_string(px, px_lists[i],
                                 axl_strlen(px_lists[i])) == AXL_OK;
    }
    ok3 = ok3 && axl_string_append_len(px, tail2, sizeof tail2) == AXL_OK;
    test_check(ok3, "ssh kex guess: built a prefix-collision KEXINIT");

    AxlSshKexinitPeer px_peer = { .guessed_wrong = false,
                                  .strict_kex = false };
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(px),
                                      axl_string_len(px), &px_peer)
                   == AXL_OK && px_peer.guessed_wrong,
               "ssh kex guess: a name we are only a PREFIX of is not ours");
    axl_string_free(px);
}

/* Build a peer KEXINIT whose kex_algorithms slot is @a kex_list, every other
   slot ours, and first_kex_packet_follows clear. Caller frees. */
static AxlString *
ssh_build_kexinit_kex(const char *kex_list)
{
    const char *lists[10];
    for (int i = 0; i < 10; i++) {
        lists[i] = ssh_kexinit_ok[i];
    }
    lists[0] = kex_list;
    return ssh_build_kexinit(lists);
}

static void
test_ssh_kexinit_strict_kex(void)
{
    /* OpenSSH PROTOCOL section 1.10, the Terrapin (CVE-2023-48795)
       mitigation. Two halves: we must ANNOUNCE ours in the initial KEXINIT,
       and we must SEE the peer's. */
    AxlString *b = axl_string_new("");
    if (b == NULL || axl_ssh_kexinit_build(b, true) != AXL_OK) {
        test_check(false, "ssh strict kex: build");
        axl_string_free(b);
        return;
    }
    const uint8_t *d   = (const uint8_t *)axl_string_data(b);
    size_t         len = axl_string_len(b);

    /* The SERVER name, in the built offer. Exact bytes, found by searching
       the payload: a name-list is length-prefixed, so a substring search over
       the whole message is the cheapest honest check that it is in there. */
    static const char WANT[] = "curve25519-sha256,kex-strict-s-v00@openssh.com";
    bool found = false;
    for (size_t i = 0; i + sizeof WANT - 1 <= len; i++) {
        if (axl_memcmp(d + i, WANT, sizeof WANT - 1) == 0) {
            found = true;
            break;
        }
    }
    test_check(found,
               "ssh strict kex: our KEXINIT offers kex-strict-s-v00@openssh.com");

    /* And our real kex name is still FIRST, so the pseudo-name did not steal
       the slot RFC 4253 section 7.1's guess test reads. */
    AxlSshKexinitPeer self = { .guessed_wrong = true,
                               .strict_kex = true };
    test_check(axl_ssh_kexinit_select(d, len, &self) == AXL_OK
                   && !self.guessed_wrong,
               "ssh strict kex: our own offer still selects, kex name first");
    /* We do not send the CLIENT name, so our own offer must not look like a
       peer asking for strict KEX. */
    test_check(!self.strict_kex,
               "ssh strict kex: our own offer carries no client pseudo-name");
    axl_string_free(b);

    /* CONTROL: a peer with no pseudo-name is reported as not strict. */
    AxlString *plain = ssh_build_kexinit_kex("curve25519-sha256");
    AxlSshKexinitPeer pp = { .guessed_wrong = false,
                             .strict_kex = true };
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(plain),
                                      axl_string_len(plain), &pp) == AXL_OK
                   && !pp.strict_kex,
               "ssh strict kex: a peer without the name is not strict");
    axl_string_free(plain);

    /* The real client's shape: ours among many, with ext-info-c and the
       pseudo-name last, exactly as OpenSSH 9.9p1 sends it. */
    AxlString *real = ssh_build_kexinit_kex(
        "mlkem768x25519-sha256,curve25519-sha256,ecdh-sha2-nistp256,"
        "ext-info-c,kex-strict-c-v00@openssh.com");
    AxlSshKexinitPeer rp = { .guessed_wrong = false,
                             .strict_kex = false };
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(real),
                                      axl_string_len(real), &rp) == AXL_OK
                   && rp.strict_kex,
               "ssh strict kex: the client pseudo-name is seen in a real list");
    axl_string_free(real);

    /* A name we are only a PREFIX of must not count. Membership is exact
       between separators, and a peer offering kex-strict-c-v00@openssh.com.x
       has asked for nothing. */
    AxlString *near = ssh_build_kexinit_kex(
        "curve25519-sha256,kex-strict-c-v00@openssh.com.evil");
    AxlSshKexinitPeer np = { .guessed_wrong = false,
                             .strict_kex = true };
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(near),
                                      axl_string_len(near), &np) == AXL_OK
                   && !np.strict_kex,
               "ssh strict kex: a longer name with ours as a prefix is not ours");
    axl_string_free(near);

    /* THE PSEUDO-NAME IS NOT A KEX METHOD. A peer offering the server name
       and nothing else has offered no key exchange at all; selecting it
       would negotiate a connection with none. */
    AxlString *only = ssh_build_kexinit_kex(
        "kex-strict-s-v00@openssh.com,kex-strict-c-v00@openssh.com");
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(only),
                                      axl_string_len(only), NULL) == AXL_ERR,
               "ssh strict kex: a pseudo-name alone is not a kex method");
    axl_string_free(only);

    /* The out-param is written on a REFUSED message too, so a caller cannot
       act on a stale true from the message before it. */
    AxlSshKexinitPeer stale = { .guessed_wrong = true,
                                .strict_kex = true };
    test_check(axl_ssh_kexinit_select(NULL, 0, &stale) == AXL_ERR
                   && !stale.strict_kex && !stale.guessed_wrong,
               "ssh strict kex: a refused message clears the out-param");
}

static void
test_ssh_secure_zero(void)
{
    /* The scrub every SSH key buffer goes through on its way back to the UEFI
       pool. Worth a test because it is the kind of primitive that gets
       "simplified" into axl_memset -- which the compiler may elide precisely
       when the storage is dead, i.e. exactly here. */
    uint8_t buf[64];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)(i + 1);
    }
    axl_ssh_secure_zero(buf, sizeof buf);
    bool all_zero = true;
    for (size_t i = 0; i < sizeof buf; i++) {
        if (buf[i] != 0) {
            all_zero = false;
        }
    }
    test_check(all_zero, "ssh scrub: every byte is zeroed");

    /* Scrubs exactly the span asked for and not a byte more -- an
       off-by-one here would corrupt whatever sits after a key. */
    uint8_t edge[8];
    for (size_t i = 0; i < sizeof edge; i++) {
        edge[i] = 0xAA;
    }
    axl_ssh_secure_zero(edge, 4);
    test_check(edge[0] == 0 && edge[3] == 0 && edge[4] == 0xAA
                   && edge[7] == 0xAA,
               "ssh scrub: stops exactly at the requested length");

    /* A zero length must touch nothing. */
    axl_ssh_secure_zero(edge + 4, 0);
    test_check(edge[4] == 0xAA, "ssh scrub: length 0 writes nothing");

    /* And the AxlString wrapper scrubs before it frees. The buffer is read
       back BEFORE the free, which is the only point at which reading it is
       defined -- so this pins the scrub, not the free. */
    AxlString *sec = axl_string_new("");
    if (sec != NULL
        && axl_string_append_len(sec, "topsecretkeymaterial", 20) == AXL_OK) {
        char *data = (char *)axl_string_data(sec);
        axl_ssh_secure_zero(data, axl_string_len(sec));
        test_check(data[0] == 0 && data[19] == 0,
                   "ssh scrub: an AxlString's bytes are cleared in place");
    } else {
        test_check(false, "ssh scrub: AxlString setup");
    }
    axl_ssh_string_free_secure(sec);
    test_survived("ssh scrub: free_secure on a live string");
    axl_ssh_string_free_secure(NULL);
    test_survived("ssh scrub: free_secure(NULL) is safe");
}

static void
test_ssh_blob_parsers(void)
{
    /* Round-trip against the BUILDERS already in the tree: a parser tested
       only against its own output would agree with a builder that is wrong.
       test_ssh_blobs() above pins the builder's bytes independently, so
       chaining onto it is what makes this meaningful. */
    uint8_t pub[32];
    for (unsigned i = 0; i < sizeof pub; i++) {
        pub[i] = (uint8_t)(0x40 + i);
    }
    AxlString *blob = axl_string_new("");
    test_check(blob != NULL, "blob parse: builder buffer");
    test_check(axl_ssh_hostkey_blob(blob, pub) == AXL_OK,
               "blob parse: hostkey blob built");

    uint8_t got[32];
    test_check(axl_ssh_hostkey_blob_parse(
                   (const uint8_t *)axl_string_data(blob),
                   axl_string_len(blob), got) == AXL_OK,
               "blob parse: hostkey blob round-trips");
    test_check(axl_memcmp(got, pub, sizeof got) == 0,
               "blob parse: hostkey bytes recovered exactly");

    /* One trailing byte inside the blob must be refused. A parser that stops
       at the second string and ignores the tail lets an attacker append
       anything to a blob whose prefix is a key we authorize. */
    AxlString *tail = axl_string_new("");
    test_check(tail != NULL, "blob parse: tail buffer");
    test_check(axl_string_append_len(tail, axl_string_data(blob),
                                     axl_string_len(blob)) == AXL_OK
                   && axl_string_append_len(tail, "\x00", 1) == AXL_OK,
               "blob parse: tail blob built");
    test_check(axl_ssh_hostkey_blob_parse(
                   (const uint8_t *)axl_string_data(tail),
                   axl_string_len(tail), got) == AXL_ERR,
               "blob parse: a trailing byte inside the blob is refused");
    axl_string_free(tail);

    /* Truncation at every length must be refused and must never read past
       the buffer. */
    unsigned refused = 0;
    for (size_t n = 0; n < axl_string_len(blob); n++) {
        if (axl_ssh_hostkey_blob_parse(
                (const uint8_t *)axl_string_data(blob), n, got) != AXL_OK) {
            refused++;
        }
    }
    test_check(refused == axl_string_len(blob),
               "blob parse: every truncation of the blob is refused");
    axl_string_free(blob);

    uint8_t sig[64];
    for (unsigned i = 0; i < sizeof sig; i++) {
        sig[i] = (uint8_t)(0x80 + i);
    }
    AxlString *sb = axl_string_new("");
    test_check(sb != NULL, "blob parse: sig buffer");
    test_check(axl_ssh_signature_blob(sb, sig) == AXL_OK,
               "blob parse: signature blob built");
    uint8_t got64[64];
    test_check(axl_ssh_signature_blob_parse(
                   (const uint8_t *)axl_string_data(sb),
                   axl_string_len(sb), got64) == AXL_OK,
               "blob parse: signature blob round-trips");
    test_check(axl_memcmp(got64, sig, sizeof got64) == 0,
               "blob parse: signature bytes recovered exactly");
    axl_string_free(sb);
}

static void
test_ssh_userauth_signed_blob(void)
{
    /* RFC 4252 section 7's blob is string(session_id) followed by the request
       bytes VERBATIM. Pinned as exact bytes, because a server that rebuilds
       these from parsed fields can differ from the client that signed them,
       and a canonicalisation difference in a signed blob is a signature
       bypass rather than a formatting bug. */
    static const uint8_t SID[4] = { 0xde, 0xad, 0xbe, 0xef };
    static const uint8_t REQ[5] = { 50, 0x00, 0x01, 0x02, 0x03 };
    static const uint8_t WANT[13] = {
        0x00, 0x00, 0x00, 0x04, 0xde, 0xad, 0xbe, 0xef,
        50,   0x00, 0x01, 0x02, 0x03
    };

    AxlString *b = axl_string_new("");
    test_check(b != NULL, "signed blob: buffer");
    test_check(axl_ssh_userauth_signed_blob(b, SID, sizeof SID,
                                            REQ, sizeof REQ) == AXL_OK,
               "signed blob: built");
    test_check(axl_string_len(b) == sizeof WANT,
               "signed blob: exact length");
    test_check(axl_memcmp(axl_string_data(b), WANT, sizeof WANT) == 0,
               "signed blob: exact bytes");
    axl_string_free(b);

    AxlString *n = axl_string_new("");
    test_check(n != NULL, "signed blob: null-arg buffer");
    test_check(axl_ssh_userauth_signed_blob(NULL, SID, 4, REQ, 5) == AXL_ERR,
               "signed blob: NULL out is refused");
    test_check(axl_ssh_userauth_signed_blob(n, NULL, 4, REQ, 5) == AXL_ERR,
               "signed blob: NULL session id is refused");
    test_check(axl_ssh_userauth_signed_blob(n, SID, 4, NULL, 5) == AXL_ERR,
               "signed blob: NULL request is refused");
    axl_string_free(n);
}

static void
test_ssh_userauth_verify_roundtrip(void)
{
    /* The end-to-end fact P2 rests on: a signature made over the RFC 4252
       blob verifies, and every one-field mutation of that blob does not.
       Signing here uses the same axl_pk_key_sign(AXL_PK_SIG_RAW) idiom
       do_kex() uses for the exchange hash. */
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ED25519);
    test_check(k != NULL, "userauth verify: Ed25519 key generated");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. An
           unbalanced skip count is silent cross-arch ratchet drift. */
        test_skip_n(10, "no Ed25519 provider in this image");
        return;
    }

    static const uint8_t SID[32] = { 1, 2, 3, 4 };
    /* A minimal but structurally real request: 50 || string("mike")
       || string("ssh-connection") || string("publickey") || TRUE
       || string("ssh-ed25519") || string(blob). Only its BYTES matter here. */
    static const uint8_t REQ[9] = { 50, 0, 0, 0, 4, 'm', 'i', 'k', 'e' };

    AxlString *blob = axl_string_new("");
    test_check(blob != NULL, "userauth verify: blob buffer");
    test_check(axl_ssh_userauth_signed_blob(blob, SID, sizeof SID,
                                            REQ, sizeof REQ) == AXL_OK,
               "userauth verify: blob built");

    uint8_t sig[64];
    size_t  sig_len = sizeof sig;
    test_check(axl_pk_key_sign(k, (const uint8_t *)axl_string_data(blob),
                               axl_string_len(blob), AXL_PK_SIG_RAW,
                               sig, &sig_len) == AXL_OK
                   && sig_len == sizeof sig,
               "userauth verify: signed the blob");

    uint8_t pub[32];
    size_t  publen = sizeof pub;
    test_check(axl_pk_key_get_raw_public(k, pub, &publen) == AXL_OK
                   && publen == sizeof pub,
               "userauth verify: exported the raw public key");

    /* Verify through a PUBLIC-ONLY handle rebuilt from the 32 bytes, which is
       exactly what the server does with a key out of the authorized_keys
       store -- not through the private handle that made the signature. */
    AxlPkKey *pubk = axl_pk_key_from_raw_public(AXL_PK_ED25519, pub,
                                                sizeof pub);
    test_check(pubk != NULL, "userauth verify: public handle rebuilt");
    test_check(axl_pk_key_verify(pubk,
                                 (const uint8_t *)axl_string_data(blob),
                                 axl_string_len(blob), AXL_PK_SIG_RAW,
                                 sig, sizeof sig) == AXL_OK,
               "userauth verify: a real signature verifies");

    uint8_t bad[64];
    axl_memcpy(bad, sig, sizeof bad);
    bad[0] ^= 0x01;
    test_check(axl_pk_key_verify(pubk,
                                 (const uint8_t *)axl_string_data(blob),
                                 axl_string_len(blob), AXL_PK_SIG_RAW,
                                 bad, sizeof bad) != AXL_OK,
               "userauth verify: a one-bit signature flip does NOT verify");

    /* A DIFFERENT session id must not verify -- this is what binds an
       authentication to the connection it was made on and stops a signature
       captured elsewhere being replayed here. */
    uint8_t other_sid[32];
    axl_memcpy(other_sid, SID, sizeof other_sid);
    other_sid[31] ^= 0x01;
    AxlString *other = axl_string_new("");
    test_check(other != NULL, "userauth verify: replay buffer");
    test_check(axl_ssh_userauth_signed_blob(other, other_sid,
                                            sizeof other_sid,
                                            REQ, sizeof REQ) == AXL_OK,
               "userauth verify: replay blob built");
    test_check(axl_pk_key_verify(pubk,
                                 (const uint8_t *)axl_string_data(other),
                                 axl_string_len(other), AXL_PK_SIG_RAW,
                                 sig, sizeof sig) != AXL_OK,
               "userauth verify: the same signature under a different session id fails");

    axl_string_free(other);
    axl_string_free(blob);
    axl_pk_key_free(pubk);
    axl_pk_key_free(k);
}

/* The user-change audit line is the longest the server writes: the OLD name
   twice, the NEW name once, and the peer. It must arrive whole at the
   longest inputs the server can hand it -- a 63-character name each time
   (AXL_SSH_LOG_USER_MAX) and the longest peer string it writes -- in the
   buffer the server formats it into. 333 is counted independently of the
   code: the format's 126 characters, less its four %s, plus 3 x 63 plus 26.
   A buffer that falls short truncates silently, dropping the NEW name's
   tail and the closing ",ssh-connection)". */
static void
test_ssh_user_change_line(void)
{
    char line[AXL_SSH_USER_CHANGE_LINE_MAX];

    axl_ssh_format_user_change(line, sizeof line, "root", "admin",
                               "10.0.2.2 port 55420");
    test_check(axl_strcmp(line,
                          "Disconnecting authenticating user root 10.0.2.2"
                          " port 55420: Change of username or service not"
                          " allowed: (root,ssh-connection) ->"
                          " (admin,ssh-connection)") == 0,
               "user change line: sshd's shape, old name twice, new once");

    char old_name[AXL_SSH_LOG_USER_MAX];
    char new_name[AXL_SSH_LOG_USER_MAX];
    axl_memset(old_name, 'o', sizeof old_name - 1);
    old_name[sizeof old_name - 1] = '\0';
    axl_memset(new_name, 'n', sizeof new_name - 1);
    new_name[sizeof new_name - 1] = '\0';
    axl_ssh_format_user_change(line, sizeof line, old_name, new_name,
                               "255.255.255.255 port 65535");
    size_t len = axl_strlen(line);
    test_check(len == 333,
               "user change line: the worst case is 333 characters, whole");
    /* The tail is the part a short buffer loses first. */
    static const char TAIL[] = ",ssh-connection)";
    bool whole = len >= 63 + sizeof TAIL - 1
                 && axl_strcmp(line + len - (sizeof TAIL - 1), TAIL) == 0
                 && axl_memcmp(line + len - (sizeof TAIL - 1) - 63, new_name,
                               63) == 0;
    test_check(whole,
               "user change line: the worst case ends with the whole new name and \",ssh-connection)\"");
}

/* The login-grace line's REASON (P3 security re-review NEW-1, resolved
   2026-09-26). Three shapes, and the whole point is that they are told apart
   by state the server already has: resume_due says the connection was still
   waiting its turn when the reaper took it, and s->hooks says whether that
   wait was behind a hook on the stack or behind somebody who had waited
   longer.

   WHY THE FIRST ASSERTION MATTERS MOST: a connection that was NOT queued
   must get the line it has always got, to the byte. That line is exact-
   matched by test-ssh-hostile-qemu.sh's penalty scenario and by the auth-
   stall reap, and a reason appended unconditionally would blame the server
   for a peer that simply sent nothing.

   The `hooks == 0` shape is the one no integration scenario can reach --
   producing it needs a connection queued behind a LONGER WAIT rather than
   behind a hook, and then reaped in that state -- so this is its only
   coverage, which is why it is pinned here rather than only in the fixture. */
static void
test_ssh_timeout_line(void)
{
    char line[AXL_SSH_TIMEOUT_LINE_MAX];

    axl_ssh_format_timeout(line, sizeof line, "10.0.2.2 port 55418", false,
                           false, true, 47320ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection"
                          " from 10.0.2.2 port 55418") == 0,
               "timeout line: a connection that never queued gets sshd's line, unchanged");

    /* Not queued and a hook on the stack is still no reason: the connection
       was not waiting on it. */
    axl_ssh_format_timeout(line, sizeof line, "10.0.2.2 port 55418", false,
                           true, true, 47320ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection"
                          " from 10.0.2.2 port 55418") == 0,
               "timeout line: a hook on the stack alone adds no reason");

    axl_ssh_format_timeout(line, sizeof line, "10.0.2.2 port 55404", true,
                           true, true, 47320ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection"
                          " from 10.0.2.2 port 55404 after waiting 47320 ms"
                          " behind another connection's authentication hook")
                   == 0,
               "timeout line: queued behind a hook names the hook and the wait");

    axl_ssh_format_timeout(line, sizeof line, "10.0.2.2 port 55404", true,
                           false, true, 1234ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection"
                          " from 10.0.2.2 port 55404 after waiting 1234 ms"
                          " in the authentication queue") == 0,
               "timeout line: queued with no hook on the stack names the queue");

    /* An unreadable clock must not invent a duration -- the same rule the
       login grace itself follows (axl_ssh_reap_reason). */
    axl_ssh_format_timeout(line, sizeof line, "10.0.2.2 port 55404", true,
                           true, false, 0ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection"
                          " from 10.0.2.2 port 55404 after waiting behind"
                          " another connection's authentication hook") == 0,
               "timeout line: an unmeasured wait says where, not how long");

    /* The worst case must arrive WHOLE in the buffer the server formats it
       into: the longest peer string, the longest reason, and a duration at
       the full width of a uint64. */
    axl_ssh_format_timeout(line, sizeof line, "255.255.255.255 port 65535",
                           true, true, true, 18446744073709551615ull);
    test_check(axl_strcmp(line,
                          "Timeout before authentication for connection from"
                          " 255.255.255.255 port 65535 after waiting"
                          " 18446744073709551615 ms behind another"
                          " connection's authentication hook") == 0,
               "timeout line: the worst case fits AXL_SSH_TIMEOUT_LINE_MAX whole");
}

/* The hook watchdog's two lines. They are not the same event and must not
   read as one: the SLOW line is written when a hook RETURNS over its budget
   and always lands; the STALLED line is written while one is still running
   and is best-effort, riding the reaper's sweep.

   The DEPTH is pinned at a real value here, not just at 0: it is measured by
   walking the resume queue, and a field nothing ever writes reads 0 for ever
   and looks healthy. */
static void
test_ssh_hook_hold_line(void)
{
    char line[AXL_SSH_HOOK_LINE_MAX];

    axl_ssh_format_hook_hold(line, sizeof line, false, true, 4210ull, 3,
                             "admin", "10.0.2.2 port 55404");
    test_check(axl_strcmp(line,
                          "Slow password hook: authentication was held 4210"
                          " ms, 3 connections waiting, for admin from"
                          " 10.0.2.2 port 55404") == 0,
               "hook line: a hook that RETURNED over budget, with the queue depth");

    axl_ssh_format_hook_hold(line, sizeof line, true, true, 4210ull, 3,
                             "admin", "10.0.2.2 port 55404");
    test_check(axl_strcmp(line,
                          "Stalled password hook: authentication held 4210 ms"
                          " so far, 3 connections waiting, for admin from"
                          " 10.0.2.2 port 55404") == 0,
               "hook line: a hook STILL RUNNING over budget says 'so far'");

    axl_ssh_format_hook_hold(line, sizeof line, false, false, 7ull, 0, "root",
                             "10.0.2.2 port 55410");
    test_check(axl_strcmp(line,
                          "Slow publickey hook: authentication was held 7 ms,"
                          " 0 connections waiting, for root from 10.0.2.2"
                          " port 55410") == 0,
               "hook line: the method comes from the request, as every other audit line's does");

    /* The worst case, whole: the longest method, a full-length sanitised
       user name, the longest peer string, and both numbers at full width. */
    char user[AXL_SSH_LOG_USER_MAX];
    axl_memset(user, 'u', sizeof user - 1);
    user[sizeof user - 1] = '\0';
    axl_ssh_format_hook_hold(line, sizeof line, true, false,
                             18446744073709551615ull, 4294967295u, user,
                             "255.255.255.255 port 65535");
    size_t len = axl_strlen(line);
    static const char TAIL[] = " from 255.255.255.255 port 65535";
    bool whole = len >= sizeof TAIL - 1 + 63
                 && axl_strcmp(line + len - (sizeof TAIL - 1), TAIL) == 0
                 && axl_memcmp(line + len - (sizeof TAIL - 1) - 63, user, 63)
                        == 0;
    test_check(whole,
               "hook line: the worst case ends with the whole user name and peer");
}

/* RFC 4252 section 5: a change of user name ends the connection, so the
   comparison is what every per-connection count (the password cap, the
   request cap) rests on. Pinned both ways, including the two shapes a
   careless compare gets wrong: a name that is a PREFIX of the kept one, and
   an embedded NUL that a C string compare would stop at. */
static void
test_ssh_userauth_user_changed(void)
{
    static const uint8_t ROOT[]  = { 'r', 'o', 'o', 't' };
    static const uint8_t ROOF[]  = { 'r', 'o', 'o', 'f' };
    static const uint8_t ROO[]   = { 'r', 'o', 'o' };
    static const uint8_t ROOTS[] = { 'r', 'o', 'o', 't', 's' };
    static const uint8_t NUL_A[] = { 'a', 0, 'b' };
    static const uint8_t NUL_B[] = { 'a', 0, 'c' };

    test_check(!axl_ssh_userauth_user_changed(ROOT, 4, 4, ROOT, 4),
               "user change: the same name is not a change");
    test_check(axl_ssh_userauth_user_changed(ROOT, 4, 4, ROOF, 4),
               "user change: a different name of the same length is one");
    test_check(axl_ssh_userauth_user_changed(ROOT, 4, 4, ROO, 3),
               "user change: a PREFIX of the kept name is one");
    test_check(axl_ssh_userauth_user_changed(ROOT, 4, 4, ROOTS, 5),
               "user change: the kept name as a prefix of the new one is one");
    test_check(axl_ssh_userauth_user_changed(NUL_A, 3, 3, NUL_B, 3),
               "user change: bytes past an embedded NUL still count");
    test_check(!axl_ssh_userauth_user_changed(NUL_A, 3, 3, NUL_A, 3),
               "user change: the same NUL-bearing name is not a change");

    /* Past the store: only the kept bytes and the wire length can be
       compared. Pinned as documented -- a different length is a change, and
       a difference past the kept bytes is not seen. */
    uint8_t long_a[300];
    uint8_t long_b[300];
    axl_memset(long_a, 'x', sizeof long_a);
    axl_memset(long_b, 'x', sizeof long_b);
    long_b[299] = 'y';
    test_check(axl_ssh_userauth_user_changed(long_a, 256, 300, long_a, 299),
               "user change: an over-long name of another length is one");
    test_check(!axl_ssh_userauth_user_changed(long_a, 256, 300, long_b, 300),
               "user change: over-long names equal on the kept bytes read as the same (documented)");
}

/* A signed publickey request for axl_ssh_userauth_sig_check: @a prefix,
   then string(ssh-ed25519 signature blob) over string(@a sid) || @a prefix
   -- so its signed length is @a prefix_len. @a flip corrupts one bit of the
   signature. NULL if any step fails. */
static AxlString *
sig_check_request(AxlPkKey *k, const uint8_t *sid, size_t sid_len,
                  const uint8_t *prefix, size_t prefix_len, bool flip)
{
    AxlString *blob    = axl_string_new("");
    AxlString *sigb    = axl_string_new("");
    AxlString *req     = axl_string_new("");
    uint8_t    sig[64];
    size_t     sig_len = sizeof sig;
    bool       ok      = blob != NULL && sigb != NULL && req != NULL
        && axl_ssh_userauth_signed_blob(blob, sid, sid_len, prefix,
                                        prefix_len) == AXL_OK
        && axl_pk_key_sign(k, (const uint8_t *)axl_string_data(blob),
                           axl_string_len(blob), AXL_PK_SIG_RAW, sig,
                           &sig_len) == AXL_OK
        && sig_len == sizeof sig;
    if (ok) {
        if (flip) {
            sig[0] ^= 0x01;
        }
        ok = axl_ssh_signature_blob(sigb, sig) == AXL_OK
             && axl_string_append_len(req, (const char *)prefix,
                                      prefix_len) == AXL_OK
             && axl_ssh_put_string(req, axl_string_data(sigb),
                                   axl_string_len(sigb)) == AXL_OK;
    }
    axl_string_free(blob);
    axl_string_free(sigb);
    if (!ok) {
        axl_string_free(req);
        return NULL;
    }
    return req;
}

/* WHOSE failure a failed signature check is. The server charges FORGED as an
   attack -- the forgery penalty, and `bad signature` in the audit log -- so
   a failure that was this machine's (an allocation) must never come back as
   FORGED, and one that was the peer's (a malformed or wrong signature) must
   never be excused as UNCHECKED. */
static void
test_ssh_userauth_sig_check(void)
{
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ED25519);
    test_check(k != NULL, "sig check: Ed25519 key generated");
    if (k == NULL) {
        /* Exactly the number of test_check calls below this guard. */
        test_skip_n(12, "no Ed25519 provider in this image");
        return;
    }

    static const uint8_t SID[32] = { 9, 8, 7, 6 };
    /* Only the BYTES of the request matter to the check; see
       test_ssh_userauth_verify_roundtrip for the shape they stand in for. */
    static const uint8_t REQ[9] = { 50, 0, 0, 0, 4, 'm', 'i', 'k', 'e' };

    uint8_t pub[32];
    size_t  publen = sizeof pub;
    test_check(axl_pk_key_get_raw_public(k, pub, &publen) == AXL_OK
                   && publen == sizeof pub,
               "sig check: exported the raw public key");
    AxlString *good = sig_check_request(k, SID, sizeof SID, REQ, sizeof REQ,
                                        false);
    AxlString *bad  = sig_check_request(k, SID, sizeof SID, REQ, sizeof REQ,
                                        true);
    test_check(good != NULL && bad != NULL,
               "sig check: built a signed request and a corrupted one");
    if (good == NULL || bad == NULL) {
        test_skip_n(10, "could not build the requests");
        axl_string_free(good);
        axl_string_free(bad);
        axl_pk_key_free(k);
        return;
    }
    const uint8_t *g     = (const uint8_t *)axl_string_data(good);
    size_t         g_len = axl_string_len(good);
    const uint8_t *b     = (const uint8_t *)axl_string_data(bad);
    size_t         b_len = axl_string_len(bad);

    test_check(axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g, g_len,
                                          sizeof REQ) == AXL_SSH_SIG_VALID,
               "sig check: a real signature is VALID");
    test_check(axl_ssh_userauth_sig_check(SID, sizeof SID, pub, b, b_len,
                                          sizeof REQ) == AXL_SSH_SIG_FORGED,
               "sig check: a one-bit signature flip is FORGED");
    /* Malformed: no signature string at all, and one cut a byte short. */
    test_check(axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g,
                                          sizeof REQ, sizeof REQ)
                   == AXL_SSH_SIG_FORGED,
               "sig check: a missing signature field is FORGED");
    test_check(axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g, g_len - 1,
                                          sizeof REQ) == AXL_SSH_SIG_FORGED,
               "sig check: a truncated signature field is FORGED");
    /* The peer's bytes are judged before anything is allocated, so memory
       cannot turn a malformed signature into "could not check". */
    axl_mem_fail_next_alloc(1);
    AxlSshSigCheck r = axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g,
                                                  g_len - 1, sizeof REQ);
    axl_mem_fail_next_alloc(0);
    test_check(r == AXL_SSH_SIG_FORGED,
               "sig check: a truncated signature is FORGED even with memory failing");

    /* The caller's own errors are not the peer's. */
    test_check(axl_ssh_userauth_sig_check(NULL, sizeof SID, pub, g, g_len,
                                          sizeof REQ) == AXL_SSH_SIG_UNCHECKED,
               "sig check: a NULL session id is UNCHECKED, not FORGED");
    test_check(axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g, g_len,
                                          g_len + 1) == AXL_SSH_SIG_UNCHECKED,
               "sig check: a signed length past the request is UNCHECKED");

    /* THE SWEEP. Fail the 1st, 2nd, ... allocation the check makes, until
       one is past the last it makes and the check comes back VALID. Every
       result before that is an allocation failure on OUR side and must be
       UNCHECKED -- including the two inside axl_pk_key_verify, which is the
       part a fix confined to this file would miss. */
    unsigned unchecked     = 0;
    unsigned forged        = 0;
    bool     reached_valid = false;
    for (size_t n = 1; n <= 64; n++) {
        axl_mem_fail_next_alloc(n);
        r = axl_ssh_userauth_sig_check(SID, sizeof SID, pub, g, g_len,
                                       sizeof REQ);
        axl_mem_fail_next_alloc(0);
        if (r == AXL_SSH_SIG_VALID) {
            reached_valid = true;
            break;
        }
        if (r == AXL_SSH_SIG_UNCHECKED) {
            unchecked++;
        } else {
            forged++;
        }
    }
    test_check(reached_valid,
               "sig check: the sweep reached a check with every allocation satisfied");
    test_check(forged == 0,
               "sig check: no allocation failure anywhere in the check reads as FORGED");
    /* The sweep's control: it really failed allocations. Four sites exist
       by construction -- the signed-data buffer, the key handle, and the
       verify's two -- so fewer means the injection never reached them and
       the assertion above passed over nothing. */
    test_check(unchecked >= 4,
               "sig check: the sweep failed at least the four allocations the check must make");

    axl_string_free(good);
    axl_string_free(bad);
    axl_pk_key_free(k);
}

/* A real ssh-ed25519 authorized_keys line. Public keys are not secrets; the
   private half of this pair was never kept. The 32 raw bytes below were
   derived by hand from the base64 (strip the 4+11+4 byte prefix), so the test
   pins the parser against an INDEPENDENT decoding rather than against
   itself. */
#define AUTHKEY_LINE \
    "ssh-ed25519 " \
    "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh" \
    " axl-p2-vector"

static const uint8_t AUTHKEY_RAW[32] = {
    0xc3, 0x9a, 0xd9, 0x7e, 0x33, 0xd3, 0x2f, 0x5d,
    0x43, 0x67, 0x10, 0xcc, 0x0a, 0x8a, 0x93, 0x96,
    0x43, 0xa5, 0x75, 0x89, 0x1c, 0xe2, 0x6a, 0x06,
    0xa9, 0x3c, 0x62, 0x5e, 0x51, 0x07, 0x61, 0x61
};

static void
test_ssh_authkey_parse_accepts(void)
{
    uint8_t got[AXL_SSH_ED25519_PUB_LEN];
    test_check(axl_ssh_authkey_parse(AUTHKEY_LINE, axl_strlen(AUTHKEY_LINE),
                                     got) == AXL_OK,
               "authkey parse: a real ssh-ed25519 line is accepted");
    test_check(axl_memcmp(got, AUTHKEY_RAW, sizeof got) == 0,
               "authkey parse: yields the 32 raw public key bytes");

    /* No trailing comment, and leading whitespace, are both legal. */
    static const char BARE[] =
        "   ssh-ed25519 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh";
    axl_memset(got, 0, sizeof got);
    test_check(axl_ssh_authkey_parse(BARE, sizeof BARE - 1, got) == AXL_OK,
               "authkey parse: leading space and no comment are accepted");
    test_check(axl_memcmp(got, AUTHKEY_RAW, sizeof got) == 0,
               "authkey parse: same key from the bare form");
}

static void
test_ssh_authkey_parse_skips(void)
{
    uint8_t got[AXL_SSH_ED25519_PUB_LEN];
    test_check(axl_ssh_authkey_parse("", 0, got) == AXL_NOT_FOUND,
               "authkey parse: an empty line is NOT_FOUND, not an error");
    test_check(axl_ssh_authkey_parse("   \t", 4, got) == AXL_NOT_FOUND,
               "authkey parse: a whitespace-only line is NOT_FOUND");
    test_check(axl_ssh_authkey_parse("# a comment", 11, got) == AXL_NOT_FOUND,
               "authkey parse: a comment line is NOT_FOUND");
    test_check(axl_ssh_authkey_parse("  # indented comment", 20, got)
                   == AXL_NOT_FOUND,
               "authkey parse: an indented comment is NOT_FOUND");
}

static void
test_ssh_authkey_parse_refuses(void)
{
    uint8_t got[AXL_SSH_ED25519_PUB_LEN];

    test_check(axl_ssh_authkey_parse(NULL, 10, got) == AXL_INVALID,
               "authkey parse: NULL line is INVALID");
    test_check(axl_ssh_authkey_parse(AUTHKEY_LINE, 5, NULL) == AXL_INVALID,
               "authkey parse: NULL out is INVALID");

    /* An options prefix must FAIL, not be parsed around. Silently ignoring
       `command="..."` would authorize a key its author meant to restrict --
       the single most dangerous way to be lenient in this file. */
    static const char OPTS[] =
        "command=\"reboot\" ssh-ed25519 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh";
    test_check(axl_ssh_authkey_parse(OPTS, sizeof OPTS - 1, got)
                   == AXL_INVALID,
               "authkey parse: an options prefix is REFUSED, not skipped");

    static const char RSA[] = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAB";
    test_check(axl_ssh_authkey_parse(RSA, sizeof RSA - 1, got) == AXL_INVALID,
               "authkey parse: ssh-rsa is refused -- one algorithm per slot");

    /* Isolates the outer type-field compare. Every other refusal vector here
       is ALSO caught by the base64 decode or the inner type name, so with the
       outer check removed the suite stayed green -- the sabotage was failing a
       different assertion than the one it was aimed at. This line's base64 is
       the same valid blob as AUTHKEY_LINE's, so only the first field can
       reject it. */
    static const char WRONG_TYPE[] =
        "ssh-ed448 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh";
    test_check(axl_ssh_authkey_parse(WRONG_TYPE, sizeof WRONG_TYPE - 1, got)
                   == AXL_INVALID,
               "authkey parse: a wrong type field with a VALID blob is refused");

    /* Isolates the memcmp half of the type-field check, which WRONG_TYPE
       above cannot reach: "ssh-ed448" is 9 characters, so the length half
       rejects it first, and so does every other refusal vector here
       (command="reboot" is 16, ssh-rsa is 7). "ssh-ed25518" is exactly as
       long as the real name and differs in one byte, so the compare is the
       only thing standing between this line and the store. */
    static const char SAME_LEN_TYPE[] =
        "ssh-ed25518 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh";
    test_check(axl_ssh_authkey_parse(SAME_LEN_TYPE, sizeof SAME_LEN_TYPE - 1,
                                     got) == AXL_INVALID,
               "authkey parse: an 11-character wrong type name is refused");

    /* Isolates the length half of the type-field check, and refuses something
       real while doing it. ssh-ed25519-cert-v01@openssh.com is OpenSSH's
       Ed25519 CERTIFICATE type and appears in real authorized_keys files; its
       first 11 bytes ARE "ssh-ed25519", so the memcmp agrees and only the
       length compare disagrees. Certificates are a different trust model --
       the key is vouched for by a CA rather than listed here -- and AxlSsh
       implements none of it, so a cert line must be refused rather than
       quietly treated as a bare key. The blob below is an ordinary key blob,
       not a real certificate: what is under test is the type field. */
    static const char CERT_TYPE[] =
        "ssh-ed25519-cert-v01@openssh.com "
        "AAAAC3NzaC1lZDI1NTE5AAAAIMOa2X4z0y9dQ2cQzAqKk5ZDpXWJHOJqBqk8Yl5RB2Fh";
    test_check(axl_ssh_authkey_parse(CERT_TYPE, sizeof CERT_TYPE - 1, got)
                   == AXL_INVALID,
               "authkey parse: an ssh-ed25519 CERTIFICATE type is refused");

    static const char TYPE_ONLY[] = "ssh-ed25519";
    test_check(axl_ssh_authkey_parse(TYPE_ONLY, sizeof TYPE_ONLY - 1, got)
                   == AXL_INVALID,
               "authkey parse: a type with no base64 field is refused");

    static const char BAD_B64[] = "ssh-ed25519 !!!!not-base64!!!!";
    test_check(axl_ssh_authkey_parse(BAD_B64, sizeof BAD_B64 - 1, got)
                   == AXL_INVALID,
               "authkey parse: undecodable base64 is refused");

    /* Decodes cleanly, but the blob's INNER type name is ssh-rsa. A parser
       that trusted only the outer field would accept 32 bytes of an unrelated
       key here. */
    static const char INNER_MISMATCH[] =
        "ssh-ed25519 "
        "AAAAB3NzaC1yc2EAAAAgw5rZfjPTL11DZxDMCoqTlkOldYkc4moGqTxiXlEHYWE=";
    test_check(axl_ssh_authkey_parse(INNER_MISMATCH,
                                     sizeof INNER_MISMATCH - 1, got)
                   == AXL_INVALID,
               "authkey parse: blob whose inner type is not ssh-ed25519 is refused");

    /* A base64 field longer than any legal blob must be refused BEFORE it is
       copied anywhere. 200 'A's decodes to 150 bytes. */
    char long_line[AXL_SSH_AUTHKEY_B64_MAX + 220];
    axl_memcpy(long_line, "ssh-ed25519 ", 12);
    axl_memset(long_line + 12, 'A', 200);
    test_check(axl_ssh_authkey_parse(long_line, 212, got) == AXL_INVALID,
               "authkey parse: an over-long base64 field is refused");
}

static void
test_ssh_authkeys_store(void)
{
    AxlSshAuthKeys ks;
    axl_memset(&ks, 0, sizeof ks);

    test_check(axl_ssh_authkeys_contains(&ks, AUTHKEY_RAW) == false,
               "authkeys: an empty store authorizes nobody");

    test_check(axl_ssh_authkeys_add_line(&ks, AUTHKEY_LINE,
                                         axl_strlen(AUTHKEY_LINE)) == AXL_OK,
               "authkeys: a valid line is added");
    test_check(ks.n == 1, "authkeys: one slot used");
    test_check(axl_ssh_authkeys_contains(&ks, AUTHKEY_RAW) == true,
               "authkeys: the added key is authorized");

    uint8_t other[AXL_SSH_ED25519_PUB_LEN];
    axl_memcpy(other, AUTHKEY_RAW, sizeof other);
    other[0] ^= 0x01;
    test_check(axl_ssh_authkeys_contains(&ks, other) == false,
               "authkeys: a key differing in one bit is NOT authorized");

    test_check(axl_ssh_authkeys_add_line(&ks, AUTHKEY_LINE,
                                         axl_strlen(AUTHKEY_LINE)) == AXL_OK,
               "authkeys: adding the same key twice succeeds");
    test_check(ks.n == 1,
               "authkeys: ...and consumes no second slot");

    test_check(axl_ssh_authkeys_add_line(&ks, "# comment", 9) == AXL_NOT_FOUND,
               "authkeys: a comment line reports NOT_FOUND");
    test_check(axl_ssh_authkeys_add_line(&ks, "ssh-rsa AAAA", 12)
                   == AXL_INVALID,
               "authkeys: a malformed line reports INVALID");
    test_check(ks.n == 1,
               "authkeys: neither changed the store");

    /* A correctly-formed line whose 32 bytes are not a usable public key
       (0xFF x 32 is a non-canonical encoding: y >= p). It is ACCEPTED here
       and refused at verify time -- see the note on
       axl_ssh_authkeys_add_line. Pinned because "the store admits it" is a
       real contract, and an earlier version of this test asserted the
       opposite against a vector that was merely truncated. */
    static const char NOT_CANONICAL[] =
        "ssh-ed25519 "
        "AAAAC3NzaC1lZDI1NTE5AAAAIP//////////////////////////////////////////";
    test_check(axl_ssh_authkeys_add_line(&ks, NOT_CANONICAL,
                                         sizeof NOT_CANONICAL - 1) == AXL_OK,
               "authkeys: a non-canonical key is admitted, and refused at verify");
}

/* Generates one authorized_keys line ("ssh-ed25519 <base64>") for a
   freshly created, valid, DISTINCT Ed25519 key pair -- the private half is
   discarded immediately. Generating is the only way to get many valid curve
   points without pinning that many vectors by hand. Returns a heap string
   the caller must axl_free(), or NULL if no Ed25519 provider is available
   or an allocation failed. */
static char *
test_ssh_gen_authkey_line(void)
{
    AxlPkKey *k = axl_pk_key_new(AXL_PK_ED25519);
    if (k == NULL) {
        return NULL;
    }
    uint8_t pub[AXL_SSH_ED25519_PUB_LEN];
    size_t  publen = sizeof pub;
    int     rc     = axl_pk_key_get_raw_public(k, pub, &publen);
    axl_pk_key_free(k);
    if (rc != AXL_OK || publen != sizeof pub) {
        return NULL;
    }

    /* Build the blob the same way ssh-keygen does. */
    AxlString *blob = axl_string_new("");
    if (blob == NULL) {
        return NULL;
    }
    char *b64 = NULL;
    if (axl_ssh_hostkey_blob(blob, pub) == AXL_OK) {
        b64 = axl_base64_encode(axl_string_data(blob), axl_string_len(blob));
    }
    axl_string_free(blob);
    if (b64 == NULL) {
        return NULL;
    }

    AxlString *line = axl_string_new("ssh-ed25519 ");
    char      *out  = NULL;
    if (line != NULL && axl_string_append(line, b64) == AXL_OK) {
        out = axl_strdup(axl_string_data(line));
    }
    axl_string_free(line);
    axl_free(b64);
    return out;
}

static void
test_ssh_authkeys_capacity(void)
{
    AxlSshAuthKeys ks;
    axl_memset(&ks, 0, sizeof ks);

    /* Fill every slot with a DISTINCT valid key, so the capacity limit is
       what stops us and not the duplicate check. */
    unsigned added = 0;
    for (unsigned i = 0; i < AXL_SSH_MAX_AUTHORIZED_KEYS + 1; i++) {
        char *line = test_ssh_gen_authkey_line();
        if (line == NULL) {
            break;
        }
        int lrc = axl_ssh_authkeys_add_line(&ks, line, axl_strlen(line));
        axl_free(line);
        if (lrc != AXL_OK) {
            break;
        }
        added++;
    }
    test_check(added == AXL_SSH_MAX_AUTHORIZED_KEYS,
               "authkeys: exactly 16 distinct keys fit");
    test_check(ks.n == AXL_SSH_MAX_AUTHORIZED_KEYS,
               "authkeys: the store is full");

    /* The 17th must be refused with NO_RESOURCES, not silently dropped and
       not written past the array. */
    test_check(axl_ssh_authkeys_add_line(&ks, AUTHKEY_LINE,
                                         axl_strlen(AUTHKEY_LINE))
                   == AXL_NO_RESOURCES,
               "authkeys: the 17th key is refused with NO_RESOURCES");
}

static void
test_ssh_server_authorized_keys_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "server api: loop");
    if (loop == NULL) {
        test_skip_n(8, "no loop");
        return;
    }
    /* Port 0 is never bound -- listen() is not called here. */
    AxlSshServer *s = axl_ssh_server_new(loop, 2222);
    test_check(s != NULL, "server api: server created");
    if (s == NULL) {
        test_skip_n(7, "no Ed25519 provider in this image");
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_add_authorized_key(NULL, AUTHKEY_LINE)
                   == AXL_INVALID,
               "server api: NULL server is INVALID");
    test_check(axl_ssh_server_add_authorized_key(s, NULL) == AXL_INVALID,
               "server api: NULL line is INVALID");

    test_check(axl_ssh_server_add_authorized_key(s, AUTHKEY_LINE) == AXL_OK,
               "server api: a valid line is accepted");

    /* A comment is NOT an error from add_authorized_key's perspective --
       nothing was added, and the caller asked for nothing. Distinct from a
       malformed line, which must be reported. */
    test_check(axl_ssh_server_add_authorized_key(s, "# nothing here")
                   == AXL_NOT_FOUND,
               "server api: a comment reports NOT_FOUND");
    test_check(axl_ssh_server_add_authorized_key(s, "ssh-rsa AAAA")
                   == AXL_INVALID,
               "server api: a malformed line reports INVALID");

    test_check(axl_ssh_server_load_authorized_keys(s, NULL) == AXL_INVALID,
               "server api: NULL path is INVALID");
    test_check(axl_ssh_server_load_authorized_keys(s, "fs0:\\no-such-file")
                   == AXL_ERR,
               "server api: an unreadable file is AXL_ERR");

    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

/* Falsifies axl_ssh_server_load_authorized_keys's documented transactional
   guarantee (include/axl/axl-ssh-server.h:114-118): a file with one bad line
   must leave the server's authorized keys exactly as they were, never
   partially applied.

   The observable: fill every slot but one with distinct valid keys, then
   attempt to load a two-line file whose FIRST line is a valid, distinct key
   that would consume the last slot and whose SECOND line is malformed. A
   transactional loader refuses the whole file and leaves the last slot
   free -- provable by successfully adding one more distinct key afterward.
   A loader that splices as it goes would have already consumed the last
   slot on the good first line, and the same add would fail NO_RESOURCES. */
static void
test_ssh_load_authorized_keys_is_transactional(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "load txn: loop");
    if (loop == NULL) {
        test_skip_n(8, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2224);
    test_check(s != NULL, "load txn: server created");
    if (s == NULL) {
        test_skip_n(7, "no Ed25519 provider in this image");
        axl_loop_free(loop);
        return;
    }

    unsigned filled = 0;
    for (unsigned i = 0; i < AXL_SSH_MAX_AUTHORIZED_KEYS - 1; i++) {
        char *line = test_ssh_gen_authkey_line();
        if (line == NULL) {
            break;
        }
        int rc = axl_ssh_server_add_authorized_key(s, line);
        axl_free(line);
        if (rc != AXL_OK) {
            break;
        }
        filled++;
    }
    test_check(filled == AXL_SSH_MAX_AUTHORIZED_KEYS - 1,
               "load txn: 15 distinct keys leave exactly one free slot");
    if (filled != AXL_SSH_MAX_AUTHORIZED_KEYS - 1) {
        test_skip_n(6, "could not fill 15 slots");
        axl_ssh_server_free(s);
        axl_loop_free(loop);
        return;
    }

    char *good_line = test_ssh_gen_authkey_line();
    test_check(good_line != NULL, "load txn: 16th distinct key generated");
    if (good_line == NULL) {
        test_skip_n(5, "key generation failed");
        axl_ssh_server_free(s);
        axl_loop_free(loop);
        return;
    }

    AxlString *file = axl_string_new(good_line);
    axl_free(good_line);
    bool built = file != NULL
                 && axl_string_append(file, "\n") == AXL_OK
                 && axl_string_append(file, "ssh-rsa AAAA") == AXL_OK;
    test_check(built, "load txn: partial-bad file content built");
    if (!built) {
        test_skip_n(4, "file content build failed");
        axl_string_free(file);
        axl_ssh_server_free(s);
        axl_loop_free(loop);
        return;
    }

    static const char PATH[] = "fs0:\\axl_ssh_authkeys_txn.tmp";
    bool staged = axl_file_set_contents(PATH, axl_string_data(file),
                                        axl_string_len(file)) == AXL_OK;
    axl_string_free(file);
    test_check(staged, "load txn: partial-bad file staged");
    if (!staged) {
        test_skip_n(3, "could not stage the file");
        axl_ssh_server_free(s);
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_load_authorized_keys(s, PATH) == AXL_INVALID,
               "load txn: a file with a bad second line is refused whole");
    axl_file_delete(PATH);

    char *canary_line = test_ssh_gen_authkey_line();
    test_check(canary_line != NULL, "load txn: canary key generated");
    if (canary_line == NULL) {
        test_skip_n(1, "key generation failed");
    } else {
        test_check(axl_ssh_server_add_authorized_key(s, canary_line)
                       == AXL_OK,
                   "load txn: last slot still free -- the refused load "
                   "changed nothing");
        axl_free(canary_line);
    }

    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

/* Stage @a text as UTF-16LE with a BOM -- what the UEFI Shell's `echo >`
   and `edit` write -- at @a path. ASCII text only. */
static bool
test_ssh_stage_utf16(const char *path, const char *text)
{
    size_t         n   = axl_strlen(text);
    unsigned char *buf = axl_malloc(2 + 2 * n);
    if (buf == NULL) {
        return false;
    }
    buf[0] = 0xFF;
    buf[1] = 0xFE;
    for (size_t i = 0; i < n; i++) {
        buf[2 + 2 * i]     = (unsigned char)text[i];
        buf[2 + 2 * i + 1] = 0;
    }
    bool ok = axl_file_set_contents(path, buf, 2 + 2 * n) == AXL_OK;
    axl_free(buf);
    return ok;
}

/* An authorized_keys file the Shell wrote (UTF-16LE + BOM) is read as the
   same file in UTF-8 would be: its one key takes a slot -- 15 more distinct
   keys fill the set, and a 17th has no room. UTF-16 that does not convert
   is refused whole. */
static void
test_ssh_load_authorized_keys_utf16(void)
{
    static const char PATH[] = "fs0:\\axl_ssh_authkeys_u16.tmp";
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "load utf16: loop");
    if (loop == NULL) {
        test_skip_n(6, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2226);
    test_check(s != NULL, "load utf16: server created");
    if (s == NULL) {
        test_skip_n(5, "no Ed25519 provider in this image");
        axl_loop_free(loop);
        return;
    }

    bool staged = test_ssh_stage_utf16(PATH, "# a comment\r\n" AUTHKEY_LINE "\r\n");
    test_check(staged && axl_ssh_server_load_authorized_keys(s, PATH) == AXL_OK,
               "load utf16: a UTF-16LE + BOM file (CRLF, a comment) loads");
    unsigned filled = 0;
    for (unsigned i = 0; i < AXL_SSH_MAX_AUTHORIZED_KEYS - 1; i++) {
        char *line = test_ssh_gen_authkey_line();
        int   rc   = line != NULL ? axl_ssh_server_add_authorized_key(s, line) : AXL_ERR;
        axl_free(line);
        if (rc != AXL_OK) {
            break;
        }
        filled++;
    }
    char *extra = test_ssh_gen_authkey_line();
    test_check(filled == AXL_SSH_MAX_AUTHORIZED_KEYS - 1 && extra != NULL
                   && axl_ssh_server_add_authorized_key(s, extra) == AXL_NO_RESOURCES,
               "load utf16: its key took a slot (15 more fill the set, the next has no room)");
    axl_free(extra);
    axl_file_delete(PATH);

    /* In a comment, which would otherwise be skipped -- so only the
       encoding check can refuse it. */
    static const unsigned char lone[] = { 0xFF, 0xFE, '#', 0, ' ', 0, 0x00, 0xD8, '\n', 0 };
    staged = axl_file_set_contents(PATH, lone, sizeof(lone)) == AXL_OK;
    test_check(staged && axl_ssh_server_load_authorized_keys(s, PATH) == AXL_INVALID,
               "load utf16: an unpaired surrogate, even in a comment, is refused (INVALID)");
    axl_file_delete(PATH);

    /* Out of memory anywhere in the load -- reading the file or decoding it --
       is AXL_ERR, never AXL_NO_RESOURCES, which means "more than 16 keys".
       Comments only, so the sweep exercises the read and the decode and no key
       parsing; it stops at the first n the load no longer reaches. */
    staged = test_ssh_stage_utf16(PATH, "# one\r\n# two\r\n");
    int      worst = AXL_OK;
    unsigned swept = 0;
    for (unsigned n = 1; staged && n <= 64; n++) {
        axl_mem_fail_next_alloc(n);
        int lrc = axl_ssh_server_load_authorized_keys(s, PATH);
        axl_mem_fail_next_alloc(0);
        if (lrc == AXL_OK) {
            break;
        }
        swept++;
        if (lrc != AXL_ERR) {
            worst = lrc;
        }
    }
    test_check(staged && swept >= 2 && worst == AXL_OK,
               "load utf16: out of memory reading or decoding is AXL_ERR, "
               "not NO_RESOURCES");
    axl_file_delete(PATH);

    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

static void
test_ssh_server_listen_requires_a_key(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "listen gate: loop");
    if (loop == NULL) {
        test_skip_n(2, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2223);
    test_check(s != NULL, "listen gate: server created");
    if (s == NULL) {
        test_skip_n(1, "no Ed25519 provider in this image");
        axl_loop_free(loop);
        return;
    }
    /* A publickey-only server with no authorized key can authenticate
       nobody, so binding a port offers network surface for nothing. Fail
       closed at configuration time rather than at every connection. This
       assertion does NOT depend on a working TCP stack: the key check comes
       before the bind. */
    test_check(axl_ssh_server_listen(s) != AXL_OK,
               "listen gate: listen fails with no authorized key");
    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

/* Task G: axl_ssh_server_set_login_grace's validation. The zero/NULL/bound
   checks need no network; "already listening" needs a REAL bind, so it gets
   its own server and its own axl_net_auto_init -- test-axl.sh's startup
   script runs DHCP before any test binary starts, which is enough on x64,
   but ssh-server-selftest.c's comment records that aa64/AAVMF needs the
   driver reconnect axl_net_auto_init does and DHCP alone does not reach.
   AXL_NET_NIC_AUTO short-circuits at once when x64's shell-level DHCP
   already leased an address, so this costs nothing there. */
static void
test_ssh_login_grace_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "login grace: loop");
    if (loop == NULL) {
        test_skip_n(9, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2250);
    test_check(s != NULL, "login grace: server created");
    if (s == NULL) {
        test_skip_n(8, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_set_login_grace(NULL, 30) == AXL_INVALID,
               "login grace: refuses a NULL server");
    test_check(axl_ssh_server_set_login_grace(s, 0) == AXL_INVALID,
               "login grace: refuses zero seconds");
    test_check(axl_ssh_server_set_login_grace(
                   s, AXL_SSH_LOGIN_GRACE_MAX_SEC + 1) == AXL_INVALID,
               "login grace: refuses one second over the maximum");
    test_check(axl_ssh_server_set_login_grace(s, AXL_SSH_LOGIN_GRACE_MAX_SEC)
                   == AXL_OK,
               "login grace: accepts exactly the maximum");

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_login_grace(s, 30) == AXL_ERR,
               "login grace: refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2251);
    test_check(live != NULL, "login grace: second server created");
    if (live == NULL) {
        test_skip_n(2, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "login grace: canary key generated");
    if (key == NULL) {
        test_skip_n(1, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_login_grace(live, 30) == AXL_ERR,
                   "login grace: refused once the server is listening");
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* The SSH server's demand on its loop's defer ring fits in the ring.

   A connection's next transmit run leaves only from its send callback, and
   AxlTcp schedules that callback on the defer ring first. Each connection
   holds at most TWO ring entries at a time -- its scheduled transmit flush
   or its finished send's delivery, never both, plus its scheduled channel
   flush -- and the server itself one more (schedule_server_free), so a loop
   that runs one server needs 2 x AXL_SSH_MAX_STARTUPS_MAX + 1 entries (the
   connection cap is static-asserted equal to it in axl-ssh-server.c).
   Past the ring, delivery
   falls to one-shot timers and then to the socket's close, which HANGS the
   session until it is closed: lifetime rule 6 at "Sending" in
   axl-ssh-server.c. The ring (and the 64-entry source table behind it) is
   SHARED with every other tenant of the loop -- HTTP and WebSocket servers
   on SoftBMC's one loop -- so this measures an EMPTY loop: it pins the
   ring's size against a future shrink or a raised connection cap, not the
   headroom a busy loop has left. */
/* Never runs: the loop is freed with the ring still full. */
static void
test_ssh_defer_noop(
    void *data  ///< unused
)
{
    (void)data;
}

static void
test_ssh_defer_ring_bound(void)
{
    AxlLoop *loop = axl_loop_new();
    unsigned held = 0;

    test_check(loop != NULL, "defer ring bound: loop");
    if (loop == NULL) {
        test_skip_n(1, "no loop");
        return;
    }
    while (held < 1024 && axl_defer(loop, test_ssh_defer_noop, NULL) != 0) {
        held++;
    }
    test_check(held >= 2u * AXL_SSH_MAX_STARTUPS_MAX + 1u,
               "defer ring bound: two entries per connection plus the"
               " server's own fit in a loop's defer ring");
    axl_loop_free(loop);
}

/* D4: axl_ssh_server_set_max_password_failures's validation, and its LIVE
   rule -- the one thing that separates it from every other set_ call but the
   authorized-key pair. The zero/NULL/bound checks need no network; "accepted
   while listening" needs a REAL bind, so it gets its own server and its own
   axl_net_auto_init, for the reason test_ssh_login_grace_api records.

   What this canNOT see is the per-connection copy: that needs two connections
   and a change between them, which is
   test-ssh-policy-qemu.sh's cap-live scenario. */
static void
test_ssh_max_password_failures_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "password cap: loop");
    if (loop == NULL) {
        test_skip_n(10, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2252);
    test_check(s != NULL, "password cap: server created");
    if (s == NULL) {
        test_skip_n(9, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_set_max_password_failures(NULL, 3) == AXL_INVALID,
               "password cap: refuses a NULL server");
    test_check(axl_ssh_server_set_max_password_failures(s, 0) == AXL_INVALID,
               "password cap: refuses zero -- that is not 'no passwords'");
    test_check(axl_ssh_server_set_max_password_failures(s, 1) == AXL_OK,
               "password cap: accepts 1, the smallest cap");
    test_check(axl_ssh_server_set_max_password_failures(
                   s, AXL_SSH_PASSWORD_FAILURES_MAX) == AXL_OK,
               "password cap: accepts exactly the maximum");
    test_check(axl_ssh_server_set_max_password_failures(
                   s, AXL_SSH_PASSWORD_FAILURES_MAX + 1) == AXL_INVALID,
               "password cap: refuses one over the maximum");

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_max_password_failures(s, 3) == AXL_ERR,
               "password cap: refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2253);
    test_check(live != NULL, "password cap: second server created");
    if (live == NULL) {
        test_skip_n(2, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "password cap: canary key generated");
    if (key == NULL) {
        test_skip_n(1, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        /* The inverse of the login-grace assertion above, deliberately: this
           one is ACCEPTED while listening, because the cap a connection
           enforces is copied into it at accept. */
        test_check(axl_ssh_server_set_max_password_failures(live, 5) == AXL_OK,
                   "password cap: accepted once the server is listening");
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* P4a spec section 12a.2: axl_ssh_server_set_max_startups's validation.
   Copies the shape of test_ssh_max_password_failures_api above -- loop,
   server on its own port, free, then a second server that must accept the
   setter while LISTENING -- rather than a second shape. The listening half
   matters for the same reason there: on_accept's admission check reads the
   value at every arrival, so the header promises it may be changed at any
   time, and a setter that refused once the server was up would break that
   promise with nothing else noticing. */
static void
test_ssh_max_startups_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "max startups: loop");
    if (loop == NULL) {
        test_skip_n(10, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2258);
    test_check(s != NULL, "max startups: server created");
    if (s == NULL) {
        test_skip_n(9, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_set_max_startups(NULL, 10) == AXL_INVALID,
               "max startups: NULL server is invalid");
    test_check(axl_ssh_server_set_max_startups(s, 0) == AXL_INVALID,
               "max startups: 0 is invalid");
    test_check(axl_ssh_server_set_max_startups(s, 17) == AXL_INVALID,
               "max startups: 17 is over the connection cap");
    test_check(axl_ssh_server_set_max_startups(s, 1) == AXL_OK,
               "max startups: 1 is accepted");
    test_check(axl_ssh_server_set_max_startups(s, 16) == AXL_OK,
               "max startups: 16 is accepted");

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_max_startups(s, 10) == AXL_ERR,
               "max startups: refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2259);
    test_check(live != NULL, "max startups: second server created");
    if (live == NULL) {
        test_skip_n(2, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "max startups: canary key generated");
    if (key == NULL) {
        test_skip_n(1, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_max_startups(live, 4) == AXL_OK,
                   "max startups: accepted once the server is listening");
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* P4a spec section 12a.2: axl_ssh_admit_decide() (axl-ssh-admit.h), the
   whole admission decision pulled out as a pure function, pinned exactly
   like axl_ssh_reap_reason() below is. on_accept()'s admission check calls
   this same function, so what is pinned here is the live decision.

   Falsifiers: scripts/sabotage.sh over axl-ssh-server.c --
     - `s/return have_victim ? AXL_SSH_ADMIT_EVICT : AXL_SSH_ADMIT_REFUSE;/return AXL_SSH_ADMIT_REFUSE;/`
       fails all three checks whose expected outcome is EVICT: both "...
       with a victim evicts" checks and "over a lowered cap evicts one per
       arrival" (same branch, different callers).
     - `s/nconns < max_conns \&\& npreauth < max_startups/npreauth < max_startups/`
       must fail "total full ...". */
static void
test_ssh_admit_decide(void)
{
    test_check(axl_ssh_admit_decide(0, 0, 16, 10, false)
                   == AXL_SSH_ADMIT_ACCEPT,
               "admit: an empty server accepts");
    test_check(axl_ssh_admit_decide(15, 9, 16, 10, false)
                   == AXL_SSH_ADMIT_ACCEPT,
               "admit: one below both caps accepts");
    test_check(axl_ssh_admit_decide(15, 10, 16, 10, true)
                   == AXL_SSH_ADMIT_EVICT,
               "admit: pre-auth full with a victim evicts");
    test_check(axl_ssh_admit_decide(15, 10, 16, 10, false)
                   == AXL_SSH_ADMIT_REFUSE,
               "admit: pre-auth full with no victim refuses");
    /* Total full by AUTHENTICATED connections: the pre-auth cap is not the
       limit, the total is -- and a pre-kex straggler is still evictable. */
    test_check(axl_ssh_admit_decide(16, 3, 16, 10, true)
                   == AXL_SSH_ADMIT_EVICT,
               "admit: total full with a victim evicts");
    test_check(axl_ssh_admit_decide(16, 0, 16, 10, false)
                   == AXL_SSH_ADMIT_REFUSE,
               "admit: total full of authenticated connections refuses");
    /* have_victim is consulted ONLY when full: a victim present on a server
       with room must not cost anyone their connection. */
    test_check(axl_ssh_admit_decide(4, 4, 16, 10, true)
                   == AXL_SSH_ADMIT_ACCEPT,
               "admit: room plus a victim still simply accepts");
    test_check(axl_ssh_admit_decide(15, 15, 16, 16, false)
                   == AXL_SSH_ADMIT_ACCEPT,
               "admit: max_startups 16 accepts up to the total");
    test_check(axl_ssh_admit_decide(1, 1, 16, 1, false)
                   == AXL_SSH_ADMIT_REFUSE,
               "admit: max_startups 1 refuses a second pre-auth");
    /* Over the cap already (it was lowered while connections were open). */
    test_check(axl_ssh_admit_decide(12, 12, 16, 5, true)
                   == AXL_SSH_ADMIT_EVICT,
               "admit: over a lowered cap evicts one per arrival");
}

/* NEW-1: axl_ssh_server_set_auth_hook_budget_ms's validation. It is the odd
   one out among the setters and the test is here to keep it that way: ZERO
   IS VALID, where set_login_grace and set_max_password_failures both refuse
   it. The difference is not style -- a disabled grace and a cap of nothing
   are exploitable, and this knob enforces nothing at all, so turning its
   reporting off costs only the report.

   "Accepted while listening" needs a REAL bind, so it gets its own server
   and its own axl_net_auto_init, for the reason test_ssh_login_grace_api
   records. What this canNOT see is the watchdog itself: a hook that holds
   the server past the budget needs a client, and that is
   test-ssh-hostile-hooks-qemu.sh's auth-hook-reenter. */
static void
test_ssh_auth_hook_budget_api(void)
{
    /* 12 test_check on the populated path, so the skip counts below are 11
       and 10. They were 10 and 9, copied from
       test_ssh_max_password_failures_api, which has ELEVEN -- one fewer
       value to pin, since 0 is invalid there and valid here. An image where
       the loop or the server cannot be created would then have declared one
       assertion fewer than every other, and the cross-arch ratchet would
       have drifted silently: exactly what the balanced-SKIP rule is for. */
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "hook budget: loop");
    if (loop == NULL) {
        test_skip_n(11, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2256);
    test_check(s != NULL, "hook budget: server created");
    if (s == NULL) {
        test_skip_n(10, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    test_check(axl_ssh_server_set_auth_hook_budget_ms(NULL, 1000)
                   == AXL_INVALID,
               "hook budget: refuses a NULL server");
    test_check(axl_ssh_server_set_auth_hook_budget_ms(s, 0) == AXL_OK,
               "hook budget: ACCEPTS zero -- reporting off, unlike the grace and the cap");
    test_check(axl_ssh_server_set_auth_hook_budget_ms(s, 1) == AXL_OK,
               "hook budget: accepts 1 ms");
    test_check(axl_ssh_server_set_auth_hook_budget_ms(
                   s, AXL_SSH_AUTH_HOOK_BUDGET_DEFAULT_MS) == AXL_OK,
               "hook budget: accepts the default");
    test_check(axl_ssh_server_set_auth_hook_budget_ms(
                   s, AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS) == AXL_OK,
               "hook budget: accepts exactly the maximum");
    test_check(axl_ssh_server_set_auth_hook_budget_ms(
                   s, AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS + 1) == AXL_INVALID,
               "hook budget: refuses one over the maximum");

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_auth_hook_budget_ms(s, 1000) == AXL_ERR,
               "hook budget: refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2257);
    test_check(live != NULL, "hook budget: second server created");
    if (live == NULL) {
        test_skip_n(2, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "hook budget: canary key generated");
    if (key == NULL) {
        test_skip_n(1, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        /* Like the password cap and the banner, and unlike the grace: this
           one is settable while the server accepts, because nothing
           enforces it -- a hook latches it at entry. */
        test_check(axl_ssh_server_set_auth_hook_budget_ms(live, 2500)
                       == AXL_OK,
                   "hook budget: accepted once the server is listening");
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* The session-channel setters' validation, and every axl_ssh_channel_* call
   on a handle that names no open channel. The handle half needs no client:
   0, a handle never issued, and a server that has been freed are all
   refused BEFORE anything is read, which is the contract's whole point --
   a stale handle kept by an application timer is an AXL_NOT_FOUND, not a
   use-after-free. What a REAL channel does is test-ssh-session-qemu.sh's. */
static void
test_ssh_channel_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "channel api: loop");
    if (loop == NULL) {
        test_skip_n(65, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2263);
    test_check(s != NULL, "channel api: server created");
    if (s == NULL) {
        test_skip_n(64, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    /* axl_ssh_server_set_max_sessions: 1..MAX, AXL_INVALID outside. */
    test_check(axl_ssh_server_set_max_sessions(NULL, 5) == AXL_INVALID,
               "max sessions: NULL server is invalid");
    test_check(axl_ssh_server_set_max_sessions(s, 0) == AXL_INVALID,
               "max sessions: 0 is invalid");
    test_check(axl_ssh_server_set_max_sessions(s, AXL_SSH_MAX_SESSIONS_MAX + 1u)
                   == AXL_INVALID,
               "max sessions: MAX + 1 is invalid");
    test_check(axl_ssh_server_set_max_sessions(s, 1) == AXL_OK,
               "max sessions: 1 is accepted");
    test_check(axl_ssh_server_set_max_sessions(s, AXL_SSH_MAX_SESSIONS_MAX)
                   == AXL_OK,
               "max sessions: MAX is accepted");

    /* axl_ssh_server_set_channel_write_limit. */
    test_check(axl_ssh_server_set_channel_write_limit(NULL, 1024, 4096)
                   == AXL_INVALID,
               "write limit: NULL server is invalid");
    test_check(axl_ssh_server_set_channel_write_limit(s, 0, 4096)
                   == AXL_INVALID,
               "write limit: a per-channel limit of 0 is invalid");
    test_check(axl_ssh_server_set_channel_write_limit(s, 1024, 0)
                   == AXL_INVALID,
               "write limit: a total of 0 is invalid");
    test_check(axl_ssh_server_set_channel_write_limit(s, 4097, 4096)
                   == AXL_INVALID,
               "write limit: a per-channel limit above the total is invalid");
    test_check(axl_ssh_server_set_channel_write_limit(
                   s, 1024, (size_t)AXL_SSH_CHANNEL_WRITE_TOTAL_MAX + 1u)
                   == AXL_INVALID,
               "write limit: a total above WRITE_TOTAL_MAX is invalid");
    test_check(axl_ssh_server_set_channel_write_limit(s, 4096, 4096)
                   == AXL_OK,
               "write limit: per-channel equal to the total is accepted");
    test_check(axl_ssh_server_set_channel_write_limit(
                   s, 1024, AXL_SSH_CHANNEL_WRITE_TOTAL_MAX) == AXL_OK,
               "write limit: a total of exactly WRITE_TOTAL_MAX is accepted");
    test_check(axl_ssh_server_set_channel_write_limit(
                   s, AXL_SSH_CHANNEL_WRITE_LIMIT_DEFAULT,
                   AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT) == AXL_OK,
               "write limit: the defaults are accepted");

    /* axl_ssh_server_set_channel_ops. */
    static const AxlSshChannelOps no_hooks = { 0 };
    test_check(axl_ssh_server_set_channel_ops(NULL, &no_hooks, NULL)
                   == AXL_INVALID,
               "channel ops: NULL server is invalid");
    test_check(axl_ssh_server_set_channel_ops(s, &no_hooks, s) == AXL_OK,
               "channel ops: an all-NULL struct is accepted");
    test_check(axl_ssh_server_set_channel_ops(s, NULL, NULL) == AXL_OK,
               "channel ops: NULL removes them");

    /* Handle 0: AXL_NOT_FOUND (NULL for the pointer getters), and checked
       before every argument but the server -- a NULL buffer with a length
       still says NOT_FOUND, because there is no channel to have refused. */
    uint8_t  addr[4] = { 9, 9, 9, 9 };
    uint16_t port    = 99;
    int      tag     = 0;
    test_check(axl_ssh_channel_write(s, 0, "x", 1) == AXL_NOT_FOUND,
               "channel 0: write is NOT_FOUND");
    test_check(axl_ssh_channel_write(s, 0, NULL, 5) == AXL_NOT_FOUND,
               "channel 0: write with a NULL buffer is NOT_FOUND, not INVALID");
    test_check(axl_ssh_channel_write(s, 0, NULL, 0) == AXL_NOT_FOUND,
               "channel 0: a zero-length write is NOT_FOUND");
    test_check(axl_ssh_channel_write_stderr(s, 0, "x", 1) == AXL_NOT_FOUND,
               "channel 0: write_stderr is NOT_FOUND");
    test_check(axl_ssh_channel_exit(s, 0, 0) == AXL_NOT_FOUND,
               "channel 0: exit is NOT_FOUND");
    test_check(axl_ssh_channel_close(s, 0) == AXL_NOT_FOUND,
               "channel 0: close is NOT_FOUND");
    test_check(axl_ssh_channel_set_user_data(s, 0, &tag) == AXL_NOT_FOUND,
               "channel 0: set_user_data is NOT_FOUND");
    test_check(axl_ssh_channel_user_data(s, 0) == NULL,
               "channel 0: user_data is NULL");
    test_check(axl_ssh_channel_user(s, 0) == NULL,
               "channel 0: user is NULL");
    test_check(axl_ssh_channel_principal(s, 0) == NULL,
               "channel 0: principal is NULL");
    test_check(axl_ssh_channel_peer(s, 0, addr, &port) == AXL_NOT_FOUND,
               "channel 0: peer is NOT_FOUND");
    test_check(addr[0] == 9 && addr[3] == 9 && port == 99,
               "channel 0: peer leaves addr and port untouched");
    test_check(axl_ssh_channel_peer(s, 0, NULL, NULL) == AXL_NOT_FOUND,
               "channel 0: peer with NULL outputs is NOT_FOUND (handle first)");
    size_t unc = 77;
    test_check(axl_ssh_channel_set_window_mode(
                   s, 0, AXL_SSH_CHANNEL_WINDOW_ON_CONSUME) == AXL_NOT_FOUND,
               "channel 0: set_window_mode is NOT_FOUND");
    test_check(axl_ssh_channel_set_window_mode(
                   s, 0, (AxlSshChannelWindowMode)7) == AXL_NOT_FOUND,
               "channel 0: set_window_mode with a bad mode is NOT_FOUND (handle first)");
    test_check(axl_ssh_channel_consumed(s, 0, 1) == AXL_NOT_FOUND,
               "channel 0: consumed is NOT_FOUND");
    test_check(axl_ssh_channel_consumed(s, 0, SIZE_MAX) == AXL_NOT_FOUND,
               "channel 0: consumed of SIZE_MAX is NOT_FOUND (handle first)");
    test_check(axl_ssh_channel_unconsumed(s, 0, &unc) == AXL_NOT_FOUND
                   && unc == 77,
               "channel 0: unconsumed is NOT_FOUND and leaves its output untouched");
    test_check(axl_ssh_channel_unconsumed(s, 0, NULL) == AXL_NOT_FOUND,
               "channel 0: unconsumed with a NULL output is NOT_FOUND (handle first)");

    /* A handle never issued. */
    test_check(axl_ssh_channel_write(s, 0x7fffffffu, "x", 1) == AXL_NOT_FOUND,
               "unissued channel: write is NOT_FOUND");
    test_check(axl_ssh_channel_exit(s, 0x7fffffffu, 1) == AXL_NOT_FOUND,
               "unissued channel: exit is NOT_FOUND");
    test_check(axl_ssh_channel_close(s, 0x7fffffffu) == AXL_NOT_FOUND,
               "unissued channel: close is NOT_FOUND");
    test_check(axl_ssh_channel_user_data(s, 0x7fffffffu) == NULL,
               "unissued channel: user_data is NULL");

    /* A NULL server: AXL_INVALID, the one argument checked first. */
    test_check(axl_ssh_channel_write(NULL, 1, "x", 1) == AXL_INVALID,
               "NULL server: write is INVALID");
    test_check(axl_ssh_channel_write_stderr(NULL, 1, "x", 1) == AXL_INVALID,
               "NULL server: write_stderr is INVALID");
    test_check(axl_ssh_channel_exit(NULL, 1, 0) == AXL_INVALID,
               "NULL server: exit is INVALID");
    test_check(axl_ssh_channel_close(NULL, 1) == AXL_INVALID,
               "NULL server: close is INVALID");
    test_check(axl_ssh_channel_set_user_data(NULL, 1, &tag) == AXL_INVALID,
               "NULL server: set_user_data is INVALID");
    test_check(axl_ssh_channel_peer(NULL, 1, addr, &port) == AXL_INVALID,
               "NULL server: peer is INVALID");
    test_check(axl_ssh_channel_set_window_mode(
                   NULL, 1, AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL) == AXL_INVALID,
               "NULL server: set_window_mode is INVALID");
    test_check(axl_ssh_channel_consumed(NULL, 1, 0) == AXL_INVALID,
               "NULL server: consumed is INVALID");
    test_check(axl_ssh_channel_unconsumed(NULL, 1, &unc) == AXL_INVALID
                   && unc == 77,
               "NULL server: unconsumed is INVALID and leaves its output untouched");
    test_check(axl_ssh_channel_user_data(NULL, 1) == NULL
                   && axl_ssh_channel_user(NULL, 1) == NULL
                   && axl_ssh_channel_principal(NULL, 1) == NULL,
               "NULL server: the getters are NULL");

    /* A freed server is recognised without being read: NOT_FOUND for the
       channel calls, AXL_ERR for the setters (not live). Under the DEBUG
       quarantine the freed struct is poisoned, so a version that walked its
       connection list would fault here rather than pass. */
    axl_ssh_server_free(s);
    test_check(axl_ssh_channel_write(s, 1, "x", 1) == AXL_NOT_FOUND,
               "freed server: write is NOT_FOUND");
    test_check(axl_ssh_channel_exit(s, 1, 0) == AXL_NOT_FOUND,
               "freed server: exit is NOT_FOUND");
    test_check(axl_ssh_channel_close(s, 1) == AXL_NOT_FOUND,
               "freed server: close is NOT_FOUND");
    test_check(axl_ssh_channel_user(s, 1) == NULL,
               "freed server: user is NULL");
    test_check(axl_ssh_channel_set_window_mode(
                   s, 1, AXL_SSH_CHANNEL_WINDOW_ON_CONSUME) == AXL_NOT_FOUND,
               "freed server: set_window_mode is NOT_FOUND");
    test_check(axl_ssh_channel_consumed(s, 1, 1) == AXL_NOT_FOUND,
               "freed server: consumed is NOT_FOUND");
    test_check(axl_ssh_channel_unconsumed(s, 1, &unc) == AXL_NOT_FOUND,
               "freed server: unconsumed is NOT_FOUND");
    test_check(axl_ssh_server_set_max_sessions(s, 5) == AXL_ERR,
               "max sessions: refused on a freed server");
    test_check(axl_ssh_server_set_channel_write_limit(s, 1024, 4096)
                   == AXL_ERR,
               "write limit: refused on a freed server");
    test_check(axl_ssh_server_set_channel_ops(s, &no_hooks, NULL) == AXL_ERR,
               "channel ops: refused on a freed server");

    /* Listening: the two "before listen" setters refuse, max_sessions is
       live. */
    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2264);
    test_check(live != NULL, "channel api: second server created");
    if (live == NULL) {
        test_skip_n(4, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "channel api: canary key generated");
    if (key == NULL) {
        test_skip_n(3, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_channel_ops(live, &no_hooks, NULL)
                       == AXL_ERR,
                   "channel ops: refused once the server is listening");
        test_check(axl_ssh_server_set_channel_write_limit(live, 1024, 4096)
                       == AXL_ERR,
                   "write limit: refused once the server is listening");
        test_check(axl_ssh_server_set_max_sessions(live, 3) == AXL_OK,
                   "max sessions: accepted once the server is listening");
    } else {
        test_skip_n(3, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* D9: axl_ssh_server_set_banner's validation, its "" rule, its OOM path and
   its LIVE rule. Everything about WHAT reaches a client -- once per
   connection, the previous banner surviving a failed set, a live set from
   inside a hook -- needs a real peer and lives in
   test-ssh-policy-qemu.sh.

   The two big buffers are static rather than automatic: 4 KB + 4 KB of
   stack inside one test function is not a cost worth taking in a UEFI
   image, and .bss in a test binary is free. */
static void
test_ssh_banner_api(void)
{
    static char max_banner[AXL_SSH_BANNER_MAX + 1];
    static char long_banner[AXL_SSH_BANNER_MAX + 2];

    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "banner: loop");
    if (loop == NULL) {
        test_skip_n(16, "no loop");
        return;
    }

    AxlSshServer *s = axl_ssh_server_new(loop, 2254);
    test_check(s != NULL, "banner: server created");
    if (s == NULL) {
        test_skip_n(15, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    axl_memset(max_banner, 'a', AXL_SSH_BANNER_MAX);
    max_banner[AXL_SSH_BANNER_MAX] = '\0';
    axl_memset(long_banner, 'b', AXL_SSH_BANNER_MAX + 1);
    long_banner[AXL_SSH_BANNER_MAX + 1] = '\0';

    test_check(axl_ssh_server_set_banner(NULL, "x") == AXL_INVALID,
               "banner: refuses a NULL server");
    test_check(axl_ssh_server_set_banner(s, NULL) == AXL_OK,
               "banner: NULL removes it");
    test_check(axl_ssh_server_set_banner(s, "") == AXL_OK,
               "banner: an empty string removes it");
    test_check(axl_ssh_server_set_banner(s, max_banner) == AXL_OK,
               "banner: accepts exactly AXL_SSH_BANNER_MAX bytes");
    test_check(axl_ssh_server_set_banner(s, long_banner) == AXL_INVALID,
               "banner: refuses one byte over the maximum");

    /* RFC 4252 section 5.4 makes this field UTF-8, so a banner that is not
       is a configuration error caught here rather than a sequence the
       client has to cope with. One case per refusal class
       (axl_utf8_validate's own boundaries are AxlTestString's job). */
    test_check(axl_ssh_server_set_banner(s, "\xC0\x80") == AXL_INVALID,
               "banner: refuses an overlong C0 80");
    test_check(axl_ssh_server_set_banner(s, "\xED\xA0\x80") == AXL_INVALID,
               "banner: refuses a UTF-16 surrogate");
    test_check(axl_ssh_server_set_banner(s, "\xFF") == AXL_INVALID,
               "banner: refuses FF, never a UTF-8 lead");
    test_check(axl_ssh_server_set_banner(s, "\xC3") == AXL_INVALID,
               "banner: refuses a truncated 2-byte sequence");
    test_check(axl_ssh_server_set_banner(s, "ok \xF0\x9F\x98\x80") == AXL_OK,
               "banner: accepts a valid 4-byte sequence");

    /* The copy is the only allocation this call makes, so the next one IS
       it. The previous banner staying in force is what the integration
       test checks as exact bytes; here the return is the assertion. */
    axl_mem_fail_next_alloc(1);
    test_check(axl_ssh_server_set_banner(s, "no room for this")
                   == AXL_NO_RESOURCES,
               "banner: reports AXL_NO_RESOURCES when the copy cannot be made");
    /* Disarm explicitly rather than trusting the call above to have spent
       it: an implementation that stopped allocating would leave the
       injection armed and fail the NEXT unrelated allocation instead --
       which is what happened while this test was RED against a stub, and
       it read as "the second server could not be created". */
    axl_mem_fail_next_alloc(0);

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_banner(s, "x") == AXL_ERR,
               "banner: refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2255);
    test_check(live != NULL, "banner: second server created");
    if (live == NULL) {
        test_skip_n(2, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    test_check(key != NULL, "banner: canary key generated");
    if (key == NULL) {
        test_skip_n(1, "key generation failed");
        axl_ssh_server_free(live);
        axl_loop_free(loop);
        return;
    }
    bool ready = axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_banner(live, "live") == AXL_OK,
                   "banner: accepted once the server is listening");
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }

    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* P5, midpoint review MP1: conn_drop starts the linger ONLY for a
   connection whose DISCONNECT went out on a healthy stream, whose linger has
   not started, on a running server. Every one of the sixteen combinations,
   because the one that was missing -- a stream that lost a run AFTER the
   DISCONNECT was queued -- is reachable only through a deferred drop no
   integration fixture can stage on demand. The CALL SITE is graded by
   test-ssh-disconnect-qemu.sh: brokenflush fails if conn_drop stops passing
   tx_broken, now the only guard (final review F2). */
static void
test_ssh_linger_due(void)
{
    unsigned wrong = 0;
    for (unsigned m = 0; m < 16; m++) {
        bool is_linger = (m & 1u) != 0;
        bool lingering = (m & 2u) != 0;
        bool running   = (m & 4u) != 0;
        bool broken    = (m & 8u) != 0;
        bool want      = is_linger && !lingering && running && !broken;
        if (axl_ssh_linger_due(is_linger, lingering, running, broken) != want) {
            wrong++;
        }
    }
    test_check(wrong == 0, "linger due: only a healthy, unstarted linger on a running server");
    test_check(!axl_ssh_linger_due(true, false, true, true),
               "linger due: a stream that lost a run is reset, never lingered (MP1)");
}

/* Regression guard for HIGH-2 (2026-09-07 security review): born_ms == 0
   used to mean both "authenticated, never reap" and "the monotonic clock
   is unreadable", so on a platform whose clock could not be read every
   unauthenticated connection looked already-authenticated, the login grace
   silently stopped existing, and 16 idle connections pinned the connection
   cap forever. The fix split that one sentinel into three fields; this test
   pins axl_ssh_reap_reason() (axl-ssh-reap.h), the pure function that is the
   whole decision, so a future refactor that re-merges the exemption with the
   deadline cannot pass with the suite green.

   Falsifier: scripts/sabotage.sh over axl-ssh-server.c's
   `if (authenticated) {` in axl_ssh_reap_reason() -- delete that early
   return and the exempt/expired assertions below both invert. */
static void
test_ssh_reap_reason(void)
{
    /* An authenticated connection is NEVER reaped -- not even one whose
       deadline is armed and has been expired for a long time. THIS is the
       exact shape of the collision HIGH-2 fixed: a merge that infers
       "authenticated" from the deadline fields would drop this connection. */
    test_check(axl_ssh_reap_reason(true, true, true, 999999999ull, 1000ull)
                   == NULL,
               "reap reason: authenticated exempts past an expired deadline");

    /* Still exempt with an UNARMED deadline -- the exemption must not be
       reachable through deadline_armed at all, in either direction. */
    test_check(axl_ssh_reap_reason(true, false, true, 5000ull, 1000ull)
                   == NULL,
               "reap reason: authenticated exempts even with no armed deadline");

    /* Still exempt when this reap tick's own clock read failed. */
    test_check(axl_ssh_reap_reason(true, true, false, 0ull, 1000ull) == NULL,
               "reap reason: authenticated exempts through a failed reading");

    /* Unauthenticated and the deadline was never armed (no readable clock at
       accept): fail CLOSED, unconditionally -- this is what stops an
       unreadable clock granting an unbounded grace, the direction HIGH-2's
       bug got wrong. */
    test_check(axl_strcmp0(
                   axl_ssh_reap_reason(false, false, true, 5000ull, 1000ull),
                   "no enforceable login grace")
                   == 0,
               "reap reason: unauthenticated + unarmed deadline drops");
    test_check(axl_strcmp0(
                   axl_ssh_reap_reason(false, false, false, 0ull, 0ull),
                   "no enforceable login grace")
                   == 0,
               "reap reason: unarmed deadline drops even if now is also unreadable");

    /* Unauthenticated, armed, and past the deadline: reaped. */
    test_check(axl_strcmp0(
                   axl_ssh_reap_reason(false, true, true, 1001ull, 1000ull),
                   "login grace expired")
                   == 0,
               "reap reason: unauthenticated + expired armed deadline drops");

    /* Exactly at the deadline is NOT past it -- the predicate is a strict
       ">", matching the field comment ("after which the grace has run
       out"). */
    test_check(axl_ssh_reap_reason(false, true, true, 1000ull, 1000ull)
                   == NULL,
               "reap reason: unauthenticated at the exact deadline is kept");

    /* Unauthenticated, armed, still inside the grace: kept. */
    test_check(axl_ssh_reap_reason(false, true, true, 999ull, 1000ull)
                   == NULL,
               "reap reason: unauthenticated within an armed deadline is kept");

    /* Unauthenticated, armed, but THIS reap tick's clock read failed: kept.
       A transient failure here must never manufacture an expiry -- the
       previous predicate's `now - born_ms` underflowed to ~2^64 on exactly
       this reading and dropped every unauthenticated connection in flight. */
    test_check(axl_ssh_reap_reason(false, true, false, 0ull, 1000ull) == NULL,
               "reap reason: a failed reap-time clock read never drops");
}

/* Task G contract review, second pass: the sub-REAP_INTERVAL_MS branch of
   axl_ssh_reap_tick_ms() (axl-ssh-reap.h) -- the finer tick a grace under
   10 s derives -- had NO coverage anywhere in the tree. Every integration
   scenario uses a grace of 10 s or more (test-ssh-hostile-qemu.sh's
   GRACE_SEC is 40), so all of them take the "stays at the 5 s ceiling"
   path; a regression that made this function always return that ceiling,
   silently reverting the whole feature for any short grace, would have
   passed every test that existed before this one. Pinned with EXACT
   values, both sides of the exact boundary (9 vs 10) and at the
   library's default (40, unaffected) and the setter's own maximum (3600,
   also unaffected).

   Falsifier: scripts/sabotage.sh over axl-ssh-server.c's
   axl_ssh_reap_tick_ms() body, replacing it with `return
   REAP_INTERVAL_MS;` -- the 1 s and 9 s assertions below both fail. */
static void
test_ssh_reap_tick_ms(void)
{
    test_check(axl_ssh_reap_tick_ms(1u, 0u, 0u) == 500u,
               "reap tick: a 1 s grace ticks at half of it, 500 ms");
    test_check(axl_ssh_reap_tick_ms(9u, 0u, 0u) == 4500u,
               "reap tick: a 9 s grace ticks at half of it, 4500 ms");
    test_check(axl_ssh_reap_tick_ms(10u, 0u, 0u) == 5000u,
               "reap tick: a 10 s grace already ticks at the 5000 ms"
               " ceiling");
    test_check(axl_ssh_reap_tick_ms(40u, 0u, 0u) == 5000u,
               "reap tick: 40 s (test-ssh-hostile-qemu.sh's GRACE_SEC)"
               " ticks at the ceiling, unchanged from before Task G");
    test_check(axl_ssh_reap_tick_ms(AXL_SSH_LOGIN_GRACE_DEFAULT_SEC, 0u, 0u) == 5000u,
               "reap tick: the shipped default (60) still ticks at the"
               " ceiling, exactly as it always has");
    test_check(axl_ssh_reap_tick_ms(AXL_SSH_LOGIN_GRACE_MAX_SEC, 0u, 0u) == 5000u,
               "reap tick: the setter's own maximum still ticks at the"
               " ceiling, not at half of one hour");
}

/* ------------------------------------------------------------------------ */
/* Idle decisions (src/net/axl-ssh-idle.h)                                  */
/* ------------------------------------------------------------------------ */

static const AxlSshIdleConn IDLE_CONN_DEFAULT = { false, true, false };

/* Start a state whose padding is deterministic, so memcmp can pin "unchanged". */
static void
idle_begin(AxlSshIdle *d, uint32_t unused_sec, uint32_t alive_sec,
           uint32_t alive_max)
{
    axl_memset(d, 0, sizeof(*d));
    axl_ssh_idle_start(d, unused_sec, alive_sec, alive_max, true, 1000ull);
}

static AxlSshIdleAct
idle_tick(AxlSshIdle *d, uint64_t now)
{
    return axl_ssh_idle_tick(d, &IDLE_CONN_DEFAULT, true, now);
}

/* sshd's client_alive_check() / serverloop.c:233-241 with the interval off:
   nothing probes and the count never moves. */
static void
test_ssh_idle_alive_off(void)
{
    AxlSshIdle d;
    idle_begin(&d, 0u, 0u, 3u);
    test_check(idle_tick(&d, 1000000ull) == AXL_SSH_IDLE_KEEP,
               "idle alive off: KEEP at any time");
    test_check(idle_tick(&d, 99999999ull) == AXL_SSH_IDLE_KEEP,
               "idle alive off: KEEP much later");
    test_check(d.unanswered == 0u, "idle alive off: the count stays 0");
}

/* The cadence of serverloop.c:235 (>=) and :113-118 (++ before the compare):
   count_max probes go out, the next due check drops. */
static void
test_ssh_idle_probe_cadence(void)
{
    AxlSshIdle d;
    idle_begin(&d, 0u, 60u, 3u);
    test_check(idle_tick(&d, 60999ull) == AXL_SSH_IDLE_KEEP,
               "idle cadence: KEEP one ms before the interval");
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE,
               "idle cadence: PROBE exactly at the interval (>=)");
    test_check(d.heard_ms == 61000ull && d.unanswered == 1u,
               "idle cadence: a probe sets heard = now and count 1");
    test_check(idle_tick(&d, 120999ull) == AXL_SSH_IDLE_KEEP,
               "idle cadence: KEEP one ms before the second probe");
    test_check(idle_tick(&d, 121000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 2u,
               "idle cadence: second PROBE, count 2");
    test_check(idle_tick(&d, 181000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 3u,
               "idle cadence: third PROBE, count 3");
    test_check(idle_tick(&d, 241000ull) == AXL_SSH_IDLE_DROP_SILENT,
               "idle cadence: DROP_SILENT at 4 > 3");
}

/* packet.c / serverloop.c:239-240: bytes reset the clock, not the count. */
static void
test_ssh_idle_heard_resets_clock_only(void)
{
    AxlSshIdle d;
    idle_begin(&d, 0u, 60u, 3u);
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE,
               "idle heard: first PROBE");
    test_check(d.bytes_ms == 1000ull,
               "idle heard: a probe moves the probe clock, not the last-bytes"
               " time (still the start)");
    axl_ssh_idle_heard(&d, true, 90000ull);
    test_check(d.heard_ms == 90000ull && d.unanswered == 1u,
               "idle heard: the clock moved, the count did not");
    test_check(d.bytes_ms == 90000ull,
               "idle heard: bytes move the last-bytes time");
    test_check(idle_tick(&d, 149999ull) == AXL_SSH_IDLE_KEEP,
               "idle heard: KEEP one ms before the new deadline");
    test_check(idle_tick(&d, 150000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 2u,
               "idle heard: PROBE at the new deadline, count 2");
}

/* packet.c:1814: a decoded packet resets the count, not the clock. */
static void
test_ssh_idle_answered_resets_count_only(void)
{
    AxlSshIdle d;
    idle_begin(&d, 0u, 60u, 3u);
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE
                   && idle_tick(&d, 121000ull) == AXL_SSH_IDLE_PROBE
                   && idle_tick(&d, 181000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 3u,
               "idle answered: three PROBEs, count 3");
    axl_ssh_idle_answered(&d);
    test_check(d.unanswered == 0u && d.heard_ms == 181000ull,
               "idle answered: the count is 0, the clock is unchanged");
    test_check(idle_tick(&d, 241000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 1u,
               "idle answered: the next due tick PROBEs, it does not drop");
}

/* serverloop.c:113-114: count_max 0 short-circuits; probe forever. */
static void
test_ssh_idle_count_max_zero(void)
{
    AxlSshIdle d;
    uint32_t   probes = 0;
    uint32_t   other = 0;
    uint64_t   now = 1000ull;
    idle_begin(&d, 0u, 60u, 0u);
    for (uint32_t i = 0; i < 1000u; i++) {
        now += 60000ull;
        if (idle_tick(&d, now) == AXL_SSH_IDLE_PROBE) {
            probes++;
        } else {
            other++;
        }
    }
    test_check(probes == 1000u && other == 0u,
               "idle count_max 0: all 1000 due ticks PROBE, none drops");
    test_check(d.unanswered == 0u, "idle count_max 0: the count never moves");
}

/* count_max UINT32_MAX never drops: the count is a uint32_t, so it wraps
   to 0 before it can exceed the largest value it can be compared against.
   It probes forever, as count_max 0 does. */
static void
test_ssh_idle_count_max_largest_never_drops(void)
{
    AxlSshIdle d;
    idle_begin(&d, 0u, 60u, UINT32_MAX);
    d.unanswered = UINT32_MAX - 1u;
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == UINT32_MAX,
               "idle count_max UINT32_MAX: the count reaches it -> PROBE");
    test_check(idle_tick(&d, 121000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 0u,
               "idle count_max UINT32_MAX: the next wraps to 0 -> PROBE, "
               "never DROP");
}

/* The reaper can tick one connection twice at one instant (it rescans
   from the head after a drop): the second tick must change nothing. */
static void
test_ssh_idle_same_now_is_idempotent(void)
{
    AxlSshIdle     d;
    AxlSshIdle     before;
    AxlSshIdleConn busy = { true, true, false };
    idle_begin(&d, 0u, 60u, 3u);
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE,
               "idle same-now: PROBE");
    before = d;
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_KEEP,
               "idle same-now: the second tick at the same now is KEEP");
    test_check(axl_memcmp(&d, &before, sizeof(d)) == 0,
               "idle same-now: the state is byte-identical");

    idle_begin(&d, 300u, 0u, 0u);
    test_check(axl_ssh_idle_tick(&d, &busy, true, 200000ull)
                   == AXL_SSH_IDLE_KEEP,
               "idle same-now: KEEP with a channel in use");
    before = d;
    test_check(axl_ssh_idle_tick(&d, &busy, true, 200000ull)
                   == AXL_SSH_IDLE_KEEP
                   && axl_memcmp(&d, &before, sizeof(d)) == 0,
               "idle same-now: in use, the second tick is KEEP and unchanged");
}

/* Design deviation: a paused peer cannot receive a probe, so none is queued;
   the count and the clock still advance. */
static void
test_ssh_idle_output_paused(void)
{
    AxlSshIdle     d;
    AxlSshIdleConn paused = { false, false, false };
    idle_begin(&d, 0u, 60u, 3u);
    test_check(axl_ssh_idle_tick(&d, &paused, true, 61000ull)
                   == AXL_SSH_IDLE_KEEP,
               "idle paused: KEEP, nothing is sent, at 61000");
    test_check(d.unanswered == 1u && d.heard_ms == 61000ull,
               "idle paused: the count is 1 and heard = 61000");
    test_check(axl_ssh_idle_tick(&d, &paused, true, 121000ull)
                   == AXL_SSH_IDLE_KEEP,
               "idle paused: KEEP at 121000");
    test_check(axl_ssh_idle_tick(&d, &paused, true, 181000ull)
                   == AXL_SSH_IDLE_KEEP && d.unanswered == 3u,
               "idle paused: KEEP at 181000, count 3");
    test_check(axl_ssh_idle_tick(&d, &paused, true, 241000ull)
                   == AXL_SSH_IDLE_DROP_SILENT,
               "idle paused: DROP_SILENT at 241000, the count exceeded");
}

/* Design deviation: with the receive parked by our own hook, the peer's
   answers sit unread because of us; no probe, no count. */
static void
test_ssh_idle_input_gated(void)
{
    AxlSshIdle     d;
    AxlSshIdleConn gated = { false, true, true };
    idle_begin(&d, 0u, 60u, 3u);
    test_check(axl_ssh_idle_tick(&d, &gated, true, 61000ull)
                       == AXL_SSH_IDLE_KEEP
                   && axl_ssh_idle_tick(&d, &gated, true, 121000ull)
                          == AXL_SSH_IDLE_KEEP
                   && axl_ssh_idle_tick(&d, &gated, true, 181000ull)
                          == AXL_SSH_IDLE_KEEP
                   && axl_ssh_idle_tick(&d, &gated, true, 241000ull)
                          == AXL_SSH_IDLE_KEEP,
               "idle gated: KEEP at every due time");
    test_check(d.unanswered == 0u && d.heard_ms == 1000ull,
               "idle gated: nothing counted, the clock untouched");
    test_check(idle_tick(&d, 241000ull) == AXL_SSH_IDLE_PROBE
                   && d.unanswered == 1u,
               "idle gated: ungated, the overdue probe goes out, count 1");
}

/* serverloop.c:246-247: strictly greater than the expiry. */
static void
test_ssh_idle_unused(void)
{
    AxlSshIdle d;
    idle_begin(&d, 300u, 0u, 0u);
    test_check(idle_tick(&d, 301000ull) == AXL_SSH_IDLE_KEEP,
               "idle unused: KEEP at exactly the expiry (strict >)");
    test_check(idle_tick(&d, 301001ull) == AXL_SSH_IDLE_DROP_UNUSED,
               "idle unused: DROP_UNUSED one ms past the expiry");

    idle_begin(&d, 0u, 0u, 0u);
    test_check(idle_tick(&d, 99999999ull) == AXL_SSH_IDLE_KEEP,
               "idle unused off: never drops");
}

/* serverloop.c:246: a channel in use refreshes the expiry on every pass. */
static void
test_ssh_idle_unused_in_use(void)
{
    AxlSshIdle     d;
    AxlSshIdleConn busy = { true, true, false };
    idle_begin(&d, 300u, 0u, 0u);
    test_check(axl_ssh_idle_tick(&d, &busy, true, 200000ull)
                       == AXL_SSH_IDLE_KEEP
                   && axl_ssh_idle_tick(&d, &busy, true, 400000ull)
                          == AXL_SSH_IDLE_KEEP,
               "idle in use: KEEP at 200000 and 400000");
    test_check(d.unused_expiry_ms == 700000ull,
               "idle in use: the expiry is 400000 + 300000");
    test_check(idle_tick(&d, 700000ull) == AXL_SSH_IDLE_KEEP,
               "idle in use: not in use, KEEP at the expiry");
    test_check(idle_tick(&d, 700001ull) == AXL_SSH_IDLE_DROP_UNUSED,
               "idle in use: DROP_UNUSED one ms past it");
}

/* The unused clock runs from the moment the last in-use slot is freed. */
static void
test_ssh_idle_channel_released(void)
{
    AxlSshIdle     d;
    AxlSshIdleConn busy = { true, true, false };
    idle_begin(&d, 300u, 0u, 0u);
    test_check(axl_ssh_idle_tick(&d, &busy, true, 200000ull)
                       == AXL_SSH_IDLE_KEEP
                   && d.unused_expiry_ms == 500000ull,
               "idle released: in use at 200000, expiry 500000");
    axl_ssh_idle_channel_released(&d, true, 450000ull);
    test_check(d.unused_expiry_ms == 750000ull,
               "idle released: the expiry runs from the release, 750000");
    test_check(idle_tick(&d, 750000ull) == AXL_SSH_IDLE_KEEP,
               "idle released: KEEP at 750000");
    test_check(idle_tick(&d, 750001ull) == AXL_SSH_IDLE_DROP_UNUSED,
               "idle released: DROP_UNUSED at 750001");
}

/* serverloop.c:233-250: the probe check runs first, so unresponsive wins. */
static void
test_ssh_idle_ranking(void)
{
    AxlSshIdle d;
    idle_begin(&d, 100u, 60u, 1u);
    test_check(idle_tick(&d, 61000ull) == AXL_SSH_IDLE_PROBE,
               "idle rank: the first probe");
    test_check(idle_tick(&d, 121000ull) == AXL_SSH_IDLE_DROP_SILENT,
               "idle rank: unresponsive and unused both due, DROP_SILENT wins");

    idle_begin(&d, 100u, 60u, 5u);
    test_check(idle_tick(&d, 101001ull) == AXL_SSH_IDLE_DROP_UNUSED,
               "idle rank: unused and a probe both due, the drop wins");
}

/* An unreadable clock decides nothing and changes nothing. */
static void
test_ssh_idle_clock_failure(void)
{
    AxlSshIdle d;
    AxlSshIdle before;
    idle_begin(&d, 300u, 60u, 3u);
    before = d;
    test_check(axl_ssh_idle_tick(&d, &IDLE_CONN_DEFAULT, false, 99999999ull)
                   == AXL_SSH_IDLE_KEEP,
               "idle clock: a failed reading at a tick is KEEP");
    test_check(axl_memcmp(&d, &before, sizeof(d)) == 0,
               "idle clock: and the state is unchanged");
    axl_ssh_idle_heard(&d, false, 5000ull);
    axl_ssh_idle_channel_released(&d, false, 5000ull);
    test_check(axl_memcmp(&d, &before, sizeof(d)) == 0,
               "idle clock: heard/released with no clock change nothing");

    axl_memset(&d, 0, sizeof(d));
    axl_ssh_idle_start(&d, 300u, 60u, 3u, false, 0ull);
    test_check(!d.armed, "idle clock: start without a clock is unarmed");
    test_check(idle_tick(&d, 99999999ull) == AXL_SSH_IDLE_KEEP
                   && idle_tick(&d, 999999999ull) == AXL_SSH_IDLE_KEEP,
               "idle clock: an unarmed state is KEEP forever");
}

/* The reaper tick is the finest of 5000, half the grace, half the unused
   timeout and half the alive interval; a period of 0 is ignored. */
static void
test_ssh_idle_reap_tick_derivation(void)
{
    test_check(axl_ssh_reap_tick_ms(60u, 0u, 0u) == 5000u,
               "reap tick: grace 60 only, the 5000 ms ceiling");
    test_check(axl_ssh_reap_tick_ms(60u, 300u, 60u) == 5000u,
               "reap tick: coarse periods leave the ceiling");
    test_check(axl_ssh_reap_tick_ms(60u, 3u, 0u) == 1500u,
               "reap tick: unused 3 s ticks at 1500 ms");
    test_check(axl_ssh_reap_tick_ms(60u, 0u, 2u) == 1000u,
               "reap tick: alive 2 s ticks at 1000 ms");
    test_check(axl_ssh_reap_tick_ms(4u, 300u, 60u) == 2000u,
               "reap tick: a 4 s grace wins at 2000 ms");
    test_check(axl_ssh_reap_tick_ms(1u, 0u, 0u) == 500u,
               "reap tick: a 1 s grace ticks at 500 ms");
    test_check(axl_ssh_reap_tick_ms(60u, 3u, 2u) == 1000u,
               "reap tick: the finest of the three, 1000 ms");
    test_check(axl_ssh_reap_tick_ms(60u, AXL_SSH_IDLE_MAX_SEC,
                                    AXL_SSH_IDLE_MAX_SEC) == 5000u,
               "reap tick: the 30 day maximum does not overflow");
}

/* Pins the per-user minimum failure time against values computed outside
   the C under test (Python hashlib, plan Task 1). */
static void
test_ssh_fail_delay_target(void)
{
    uint8_t secret[AXL_SSH_TIMING_SECRET_LEN];
    for (size_t i = 0; i < sizeof secret; i++) {
        secret[i] = (uint8_t)i;
    }
    test_check(axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                            "admin", 5) == 6999u,
               "fail delay: admin under the test secret is 6999 us");
    test_check(axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                            "root", 4) == 8689u,
               "fail delay: root under the test secret is 8689 us");
    test_check(axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                            "", 0) == 6033u,
               "fail delay: the empty user name hashes the secret alone");
    test_check(axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                            "admin", 5)
                   == axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                                   "admin", 5),
               "fail delay: the same user always gets the same target");
    test_check(axl_ssh_fail_delay_target_us(NULL, 32, "admin", 5) == 9294u,
               "fail delay: a NULL secret returns the uniform fallback");
    test_check(axl_ssh_fail_delay_target_us(secret, sizeof secret,
                                            NULL, 5) == 9294u,
               "fail delay: a NULL user with a length returns the fallback");
}

/* Pins OpenSSH's doubling rule, including both boundaries. */
static void
test_ssh_fail_reply_delay(void)
{
    test_check(axl_ssh_fail_reply_delay_us(0, 5000) == 5000u,
               "reply delay: nothing elapsed waits the whole target");
    test_check(axl_ssh_fail_reply_delay_us(4999, 5000) == 1u,
               "reply delay: one microsecond short waits one microsecond");
    test_check(axl_ssh_fail_reply_delay_us(5000, 5000) == 0u,
               "reply delay: exactly on target waits nothing (no doubling)");
    test_check(axl_ssh_fail_reply_delay_us(6000, 5000) == 4000u,
               "reply delay: past target doubles it once");
    test_check(axl_ssh_fail_reply_delay_us(100000, 7000) == 12000u,
               "reply delay: a 100 ms check doubles 7 ms up to 112 ms");
    test_check(axl_ssh_fail_reply_delay_us(5000000, 7000) == 2168000u,
               "reply delay: exactly 5 s elapsed is still delayed");
    test_check(axl_ssh_fail_reply_delay_us(5000001, 7000) == 0u,
               "reply delay: past 5 s elapsed adds nothing");
    test_check(axl_ssh_fail_reply_delay_us(0, 0) == 0u,
               "reply delay: a zero target cannot loop forever");
}

static void
test_ssh_listen_has_method(void)
{
    test_check(axl_ssh_listen_has_method(0, false) == false,
               "listen gate: no keys and no password handler refuses");
    test_check(axl_ssh_listen_has_method(1, false) == true,
               "listen gate: one authorized key is enough");
    test_check(axl_ssh_listen_has_method(0, true) == true,
               "listen gate: a password handler alone is enough");
    test_check(axl_ssh_listen_has_method(3, true) == true,
               "listen gate: both is enough");
}

/* The argument checks and the "nothing is waiting" answer. No connection
   issues a request until P3 Task 3, so every well-formed completion here is
   AXL_NOT_FOUND by construction -- which is what makes a late completion
   safe, and what a principal-ownership bug would violate first. */
static void
test_ssh_unimplemented(void)
{
    /* RFC 4253 section 11.4: byte SSH_MSG_UNIMPLEMENTED, uint32
       packet_sequence_number. Five bytes, and the only thing a peer can do
       with the message is match that number against what it sent -- so the
       encoding is pinned exactly rather than parsed back with our own
       reader, which would agree with itself about a wrong endianness. */
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        test_check(false, "ssh unimplemented: alloc");
        return;
    }
    test_check(axl_ssh_unimplemented_build(m, 0x01020304u) == AXL_OK,
               "ssh unimplemented: build ok");
    static const uint8_t WANT[] = { 3, 0x01, 0x02, 0x03, 0x04 };
    test_check(axl_string_len(m) == sizeof WANT,
               "ssh unimplemented: exactly 5 bytes");
    test_check(axl_string_len(m) == sizeof WANT
                   && axl_memcmp(axl_string_data(m), WANT, sizeof WANT) == 0,
               "ssh unimplemented: byte 3 then the sequence number big-endian");
    axl_string_free(m);

    test_check(axl_ssh_unimplemented_build(NULL, 0) == AXL_ERR,
               "ssh unimplemented: NULL builder errs");

    /* CONTROL: a message number nothing implements DOES earn a reply, in
       both states, so every exclusion below is about the number it names. */
    test_check(axl_ssh_unimplemented_due(200, false),
               "ssh unimplemented: an unknown number earns a reply pre-auth");
    test_check(axl_ssh_unimplemented_due(200, true),
               "ssh unimplemented: an unknown number earns a reply post-auth");
    /* RFC 4254 section 4's GLOBAL_REQUEST: still classified due here, because
       this function classifies messages generically -- routing, not this
       function, now answers it (axl_ssh_global_request_parse +
       send_request_failure, ST_SESSION). */
    test_check(axl_ssh_unimplemented_due(80, true),
               "ssh unimplemented: 80 is still classified due; routing, not this function, answers it");

    /* RFC 4253 section 11: these are recognised and must not be answered. */
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_DISCONNECT, false),
               "ssh unimplemented: DISCONNECT is never answered");
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_IGNORE, false),
               "ssh unimplemented: SSH_MSG_IGNORE is ignored, not answered");
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_DEBUG, false),
               "ssh unimplemented: SSH_MSG_DEBUG is ignored, not answered");
    /* The ping-pong: two conforming peers answering each other for ever. */
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_UNIMPLEMENTED, false),
               "ssh unimplemented: UNIMPLEMENTED is never answered with one");

    /* RFC 4252 section 5.1: after a successful login, userauth is IGNORED --
       and an UNIMPLEMENTED reply is not ignoring it. The whole range, not
       just USERAUTH_REQUEST. */
    test_check(axl_ssh_unimplemented_due(AXL_SSH_MSG_USERAUTH_FIRST, false),
               "ssh unimplemented: pre-auth, an unhandled userauth number earns one");
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_USERAUTH_FIRST, true),
               "ssh unimplemented: post-auth, USERAUTH_REQUEST is silently ignored");
    test_check(!axl_ssh_unimplemented_due(AXL_SSH_MSG_USERAUTH_LAST, true),
               "ssh unimplemented: post-auth, the whole userauth range is ignored");
    test_check(axl_ssh_unimplemented_due(AXL_SSH_MSG_USERAUTH_LAST + 1, true),
               "ssh unimplemented: post-auth, one past the range earns one again");
    test_check(axl_ssh_unimplemented_due(AXL_SSH_MSG_USERAUTH_FIRST - 1, true),
               "ssh unimplemented: post-auth, one before the range earns one");
}

/* P4a: RFC 4254 section 4's GLOBAL_REQUEST, parsed to the one field a server
   that implements no global request needs -- whether to answer. */
static void
test_ssh_global_request(void)
{
    AxlString *m = axl_string_new("");
    bool want = false;
    char t80 = (char)AXL_SSH_MSG_GLOBAL_REQUEST;
    char one = 1, zero = 0, two = 2;

    /* keepalive@openssh.com, want_reply TRUE -- what ssh -o
       ServerAliveInterval sends, and what sshd answers with a FAILURE. */
    axl_string_append_len(m, &t80, 1);
    axl_ssh_put_string(m, "keepalive@openssh.com", 21);
    axl_string_append_len(m, &one, 1);
    test_check(axl_ssh_global_request_parse(
                   (const uint8_t *)axl_string_data(m), axl_string_len(m),
                   &want) == AXL_OK && want,
               "global request: want_reply 1 parses as TRUE");

    /* want_reply 2: RFC 4251 section 5, any non-zero byte is TRUE. */
    axl_string_truncate(m, axl_string_len(m) - 1);
    axl_string_append_len(m, &two, 1);
    want = false;
    test_check(axl_ssh_global_request_parse(
                   (const uint8_t *)axl_string_data(m), axl_string_len(m),
                   &want) == AXL_OK && want,
               "global request: want_reply 2 parses as TRUE");

    /* want_reply 0, with tcpip-forward's request-specific data trailing --
       the data is not read and must not be a reason to refuse. */
    axl_string_truncate(m, 0);
    axl_string_append_len(m, &t80, 1);
    axl_ssh_put_string(m, "tcpip-forward", 13);
    axl_string_append_len(m, &zero, 1);
    axl_ssh_put_string(m, "127.0.0.1", 9);
    axl_ssh_put_u32(m, 0);
    want = true;
    test_check(axl_ssh_global_request_parse(
                   (const uint8_t *)axl_string_data(m), axl_string_len(m),
                   &want) == AXL_OK && !want,
               "global request: want_reply 0 with trailing data parses as FALSE");

    /* Truncated before the boolean. */
    axl_string_truncate(m, 0);
    axl_string_append_len(m, &t80, 1);
    axl_ssh_put_string(m, "x", 1);
    test_check(axl_ssh_global_request_parse(
                   (const uint8_t *)axl_string_data(m), axl_string_len(m),
                   &want) == AXL_ERR,
               "global request: missing want_reply is refused");

    /* A name whose length runs past the message. */
    static const uint8_t overlong[] = { 80, 0, 0, 0, 9, 'x', 1 };
    test_check(axl_ssh_global_request_parse(overlong, sizeof overlong, &want)
                   == AXL_ERR,
               "global request: a name past the end is refused");

    /* Wrong type, and NULL. */
    static const uint8_t wrong[] = { 81, 0, 0, 0, 0, 1 };
    test_check(axl_ssh_global_request_parse(wrong, sizeof wrong, &want)
                   == AXL_ERR,
               "global request: a type other than 80 is refused");
    test_check(axl_ssh_global_request_parse(NULL, 6, &want) == AXL_ERR,
               "global request: NULL is refused");

    /* The reply: exactly one byte, 82. */
    axl_string_truncate(m, 0);
    test_check(axl_ssh_request_failure_build(m) == AXL_OK
                   && axl_string_len(m) == 1
                   && (uint8_t)axl_string_data(m)[0] == 82,
               "request failure: exactly the byte 82");
    axl_string_free(m);
}

static void
test_ssh_auth_log_rate(void)
{
    AxlSshAuthLogRate r = { .noted_ms = 0, .suppressed = 0,
                            .noted = false };
    uint32_t n = 99;

    /* The first failure is always announced -- an audit trail whose first
       entry is missing cannot be reconstructed from the later ones. */
    test_check(axl_ssh_auth_log_due(&r, 1000, true, &n) && n == 0,
               "ssh auth log: the first failure is announced, 0 suppressed");

    /* Inside the window: suppressed, and counted. */
    test_check(!axl_ssh_auth_log_due(&r, 1001, true, NULL),
               "ssh auth log: a second failure 1 ms later is suppressed");
    test_check(!axl_ssh_auth_log_due(&r, 1000 + AXL_SSH_AUTH_LOG_WINDOW_MS - 1,
                                     true, NULL),
               "ssh auth log: one ms short of the window is still suppressed");

    /* Exactly at the window: announced, carrying the count of what was not. */
    n = 99;
    test_check(axl_ssh_auth_log_due(&r, 1000 + AXL_SSH_AUTH_LOG_WINDOW_MS,
                                    true, &n) && n == 2,
               "ssh auth log: at the window it is announced, reporting 2 suppressed");

    /* ...and the count RESETS, so the next line does not re-report them. */
    test_check(!axl_ssh_auth_log_due(&r, 1000 + AXL_SSH_AUTH_LOG_WINDOW_MS,
                                     true, NULL),
               "ssh auth log: the next failure in the new window is suppressed");
    n = 99;
    test_check(axl_ssh_auth_log_due(&r, 1000 + 2 * AXL_SSH_AUTH_LOG_WINDOW_MS,
                                    true, &n) && n == 1,
               "ssh auth log: the suppressed count resets at each announcement");

    /* A clock that went BACKWARDS must not be read as "the window elapsed".
       axl_ssh_penalty_drop_notice takes the same view for the same reason. */
    test_check(!axl_ssh_auth_log_due(&r, 0, true, NULL),
               "ssh auth log: a backwards clock reports nothing new");

    /* An unreadable clock: the first failure still gets its line, later ones
       are suppressed rather than becoming an unbounded flood. */
    AxlSshAuthLogRate blind = { .noted_ms = 0, .suppressed = 0,
                                .noted = false };
    n = 99;
    test_check(axl_ssh_auth_log_due(&blind, 0, false, &n) && n == 0,
               "ssh auth log: a first failure with no clock is still announced");
    test_check(!axl_ssh_auth_log_due(&blind, 0, false, NULL),
               "ssh auth log: later failures with no clock are suppressed");

    /* WHAT A RECOVERED CLOCK ACTUALLY DOES, pinned on BOTH sides of the
       window. An announcement made with no reading anchors the window at
       zero, so a later readable failure is due once the clock itself reads a
       full window -- not unconditionally "at once", which is what the
       docstring used to claim. Passing exactly AXL_SSH_AUTH_LOG_WINDOW_MS
       alone could not tell the two apart; the assertion below it is the one
       that discriminates, and it fails if the claim is restored. */
    test_check(!axl_ssh_auth_log_due(&blind, AXL_SSH_AUTH_LOG_WINDOW_MS - 1,
                                     true, NULL),
               "ssh auth log: a recovered clock short of the window waits");
    n = 99;
    test_check(axl_ssh_auth_log_due(&blind, AXL_SSH_AUTH_LOG_WINDOW_MS, true,
                                    &n) && n == 2,
               "ssh auth log: a recovered clock announces at the window");
}

static void
test_ssh_password_reaches_hook(void)
{
    static const uint8_t PW[]  = { 'h', 'u', 'n', 't', 'e', 'r', '2' };
    static const uint8_t NUL[] = { 'a', 0, 'b' };

    /* CONTROL: an ordinary password DOES reach the hook, so every refusal
       below is about the thing it names and not about the predicate being
       stuck at false. */
    test_check(axl_ssh_password_reaches_hook(false, PW, sizeof PW),
               "ssh password form: an ordinary password reaches the hook");

    /* OpenSSH's PermitEmptyPasswords default is `no`, and sshd
       short-circuits before the backend. So does this. */
    test_check(!axl_ssh_password_reaches_hook(false, PW, 0),
               "ssh password form: an EMPTY password never reaches the hook");
    test_check(!axl_ssh_password_reaches_hook(false, NULL, 0),
               "ssh password form: a NULL empty password never reaches the hook");

    /* The change form, refused unexamined -- and refused even when the
       password beside it is perfectly ordinary. */
    test_check(!axl_ssh_password_reaches_hook(true, PW, sizeof PW),
               "ssh password form: the change form is refused unexamined");

    /* An embedded NUL: the hook's NUL-terminated copy would be a different
       password from the one the client sent. */
    test_check(!axl_ssh_password_reaches_hook(false, NUL, sizeof NUL),
               "ssh password form: an embedded NUL is refused");

    /* The length bound, on both sides of it -- a one-off here is a buffer
       overrun in the caller's fixed copy. */
    uint8_t *big = axl_malloc(AXL_SSH_AUTH_PASSWORD_MAX + 1);
    if (big == NULL) {
        test_check(false, "ssh password form: alloc");
        return;
    }
    axl_memset(big, 'x', AXL_SSH_AUTH_PASSWORD_MAX + 1);
    test_check(axl_ssh_password_reaches_hook(false, big,
                                             AXL_SSH_AUTH_PASSWORD_MAX),
               "ssh password form: exactly AXL_SSH_AUTH_PASSWORD_MAX is allowed");
    test_check(!axl_ssh_password_reaches_hook(false, big,
                                              AXL_SSH_AUTH_PASSWORD_MAX + 1),
               "ssh password form: one byte over the maximum is refused");
    axl_free(big);
}

static void
test_ssh_auth_api(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "auth api: loop");
    if (loop == NULL) {
        test_skip_n(17, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2224);
    test_check(s != NULL, "auth api: server created");
    if (s == NULL) {
        test_skip_n(16, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    AxlSshAuthOps ops;
    axl_memset(&ops, 0, sizeof ops);
    test_check(axl_ssh_server_set_auth_ops(NULL, &ops, NULL) == AXL_INVALID,
               "auth api: set_auth_ops refuses a NULL server");
    test_check(axl_ssh_server_set_auth_ops(s, &ops, NULL) == AXL_OK,
               "auth api: set_auth_ops accepts all-NULL hooks");
    test_check(axl_ssh_server_set_auth_ops(s, NULL, NULL) == AXL_OK,
               "auth api: set_auth_ops with NULL removes the hooks");

    int  principal_tag = 7;
    char max_banner[AXL_SSH_AUTH_BANNER_MAX + 1];
    char long_banner[AXL_SSH_AUTH_BANNER_MAX + 2];
    axl_memset(max_banner, 'a', AXL_SSH_AUTH_BANNER_MAX);
    max_banner[AXL_SSH_AUTH_BANNER_MAX] = '\0';
    axl_memset(long_banner, 'b', AXL_SSH_AUTH_BANNER_MAX + 1);
    long_banner[AXL_SSH_AUTH_BANNER_MAX + 1] = '\0';

    test_check(axl_ssh_auth_complete(NULL, 1, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_INVALID,
               "auth api: complete refuses a NULL server");
    test_check(axl_ssh_auth_complete(s, 0, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_INVALID,
               "auth api: complete refuses the zero handle");
    test_check(axl_ssh_auth_complete(s, 1, (AxlSshAuthVerdict)99, NULL, NULL)
                   == AXL_INVALID,
               "auth api: complete refuses an unknown verdict");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, &principal_tag,
                                     NULL) == AXL_INVALID,
               "auth api: a principal may not accompany DENY");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_CHANGE_FIRST,
                                     &principal_tag, NULL) == AXL_INVALID,
               "auth api: a principal may not accompany CHANGE_FIRST");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL,
                                     long_banner) == AXL_INVALID,
               "auth api: a banner one byte over the maximum is refused");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL,
                                     max_banner) == AXL_NOT_FOUND,
               "auth api: a banner of exactly the maximum is not refused");
    /* RFC 4252 section 5.4 makes this field UTF-8, and it was checked on
       axl_ssh_server_set_banner()'s path and not on this one. AXL_INVALID
       comes from the validation, which runs BEFORE the handle lookup -- so
       a refusal here is the banner, not the unknown handle (the two
       AXL_NOT_FOUND cases either side are what say so). */
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL,
                                     "\xC0\x80") == AXL_INVALID,
               "auth api: a banner with an overlong sequence is refused");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL,
                                     "bad \xFF byte") == AXL_INVALID,
               "auth api: a banner with an invalid UTF-8 lead is refused");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL,
                                     "ok \xF0\x9F\x98\x80") == AXL_NOT_FOUND,
               "auth api: a valid 4-byte sequence is not refused");
    /* "" is NO banner, as it is for axl_ssh_server_set_banner -- not an
       empty USERAUTH_BANNER on the wire. Reaching the lookup is what says
       it was accepted rather than validated away. */
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL, "")
                   == AXL_NOT_FOUND,
               "auth api: an empty banner is accepted, meaning none");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_NOT_FOUND,
               "auth api: nothing waits on handle 1");
    test_check(axl_ssh_auth_complete(s, 1, AXL_SSH_AUTH_ACCEPT,
                                     &principal_tag, NULL) == AXL_NOT_FOUND,
               "auth api: an ACCEPT for an unknown handle is not adopted");

    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

/* The handle step (P3 security review MED-4). The server issues every handle
   from ONE counter shared by all its instances through this step, so what can
   be pinned here is the step itself: it counts up by one and never lands on
   0, the invalid handle. That the counter is shared -- not per server -- is
   structural (a file-static in axl-ssh-server.c) and is covered by review,
   not by this test. */
static void
test_ssh_auth_request_next(void)
{
    AxlSshAuthRequest last = 0;
    test_check(axl_ssh_auth_request_next(&last) == 1 && last == 1,
               "request handle: the first handle is 1");
    last = 41;
    test_check(axl_ssh_auth_request_next(&last) == 42 && last == 42,
               "request handle: counts up by one");
    last = 0xFFFFFFFEu;
    test_check(axl_ssh_auth_request_next(&last) == 0xFFFFFFFFu,
               "request handle: 0xFFFFFFFF is issued");
    test_check(axl_ssh_auth_request_next(&last) == 1 && last == 1,
               "request handle: the wrap skips 0");
}

/* The live-server table is bounded, and full means axl_ssh_server_new
   REFUSES -- a server the table cannot record is one whose late completions
   could not be refused safely, so it must not exist. A slot comes back when a
   server is freed. */
static void
test_ssh_live_server_cap(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "live servers: loop");
    if (loop == NULL) {
        test_skip_n(3, "no loop");
        return;
    }
    AxlSshServer *srv[AXL_SSH_LIVE_SERVERS_MAX];
    unsigned      made = 0;
    for (unsigned i = 0; i < AXL_SSH_LIVE_SERVERS_MAX; i++) {
        srv[i] = axl_ssh_server_new(loop, (uint16_t)(2230 + i));
        if (srv[i] != NULL) {
            made++;
        }
    }
    if (made == 0) {
        test_skip_n(3, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    test_check(made == AXL_SSH_LIVE_SERVERS_MAX,
               "live servers: AXL_SSH_LIVE_SERVERS_MAX may be live at once");

    AxlSshServer *extra = axl_ssh_server_new(loop, 2240);
    test_check(extra == NULL, "live servers: one more is refused");
    axl_ssh_server_free(extra);

    axl_ssh_server_free(srv[0]);
    srv[0] = axl_ssh_server_new(loop, 2241);
    test_check(srv[0] != NULL, "live servers: a freed server's slot is reused");

    for (unsigned i = 0; i < AXL_SSH_LIVE_SERVERS_MAX; i++) {
        axl_ssh_server_free(srv[i]);
    }
    axl_loop_free(loop);
}

/* P3 security review MED-4: a completion that outlives its server. The
   natural teardown order -- axl_ssh_server_free, then axl_async_free, whose
   drain runs done callbacks -- delivers exactly this call, and it must be
   refused without the freed server being read.

   What makes it able to fail: the unit build is DEBUG, so AxlMem fills a
   freed block with 0xDF and holds it in its quarantine. A completion that
   walks the freed server's connection list reads 0xDFDFDFDFDFDFDFDF as a
   pointer and faults, instead of finding the NULL a freshly freed server
   happened to leave behind and answering AXL_NOT_FOUND by luck.

   Kept LAST in test_ssh_main: against a server that dereferences first, this
   faults the binary, and a fault here starves nothing after it. */
static void
test_ssh_auth_complete_after_free(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "complete after free: loop");
    if (loop == NULL) {
        test_skip_n(5, "no loop");
        return;
    }
    AxlSshServer *first  = axl_ssh_server_new(loop, 2225);
    AxlSshServer *second = axl_ssh_server_new(loop, 2226);
    test_check(first != NULL && second != NULL,
               "complete after free: two servers live at once");
    if (first == NULL || second == NULL) {
        test_skip_n(4, "no server (Ed25519 provider or RNG unavailable)");
        axl_ssh_server_free(first);
        axl_ssh_server_free(second);
        axl_loop_free(loop);
        return;
    }

    axl_ssh_server_free(first);
    test_check(axl_ssh_auth_complete(first, 1, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_NOT_FOUND,
               "complete after free: a completion against a freed server is"
               " refused");
    /* Arguments are still validated first, and that touches no server. */
    test_check(axl_ssh_auth_complete(first, 0, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_INVALID,
               "complete after free: a malformed call is still AXL_INVALID");

    /* Freeing ANOTHER server must not make the first look live again. */
    axl_ssh_server_free(second);
    test_check(axl_ssh_auth_complete(first, 1, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_NOT_FOUND,
               "complete after free: the first is still refused once a second"
               " server is freed");
    test_check(axl_ssh_auth_complete(second, 1, AXL_SSH_AUTH_DENY, NULL, NULL)
                   == AXL_NOT_FOUND,
               "complete after free: the second is refused too");

    axl_loop_free(loop);
}

/* A second axl_ssh_server_free for the same server is ignored, without the
   server being read. The free may now be called from inside a hook's
   dispatch and finish later, so two handlers that each turn SSH off during
   one dispatch call it twice on a server whose free has started -- and a
   caller's plain double free must not become heap corruption either.

   What makes it able to fail: as in test_ssh_auth_complete_after_free, the
   DEBUG quarantine fills the freed server with 0xDF. A second free that reads
   it removes a source from a "loop" at 0xDFDFDFDFDFDFDFDF and faults; one
   that only writes it is caught when the quarantine is drained below.

   Kept LAST in test_ssh_main, for the same reason: against a server that
   reads first, this faults the binary. */
static void
test_ssh_server_free_twice(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "free twice: loop");
    if (loop == NULL) {
        test_skip_n(1, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2227);
    if (s == NULL) {
        test_skip_n(1, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    size_t corruptions = axl_mem_corruption_count();
    axl_ssh_server_free(s);
    axl_ssh_server_free(s);
    /* Drain, so a write into the freed server is checked now, then restore
       the default 16-block ring. */
    axl_mem_set_quarantine(0);
    axl_mem_set_quarantine(16);
    test_check(axl_mem_corruption_count() == corruptions,
               "free twice: the second call touched nothing");

    axl_loop_free(loop);
}

/* ------------------------------------------------------------------------ */
/* Per-source penalties (Task P: axl_ssh_server_set_penalties)               */
/*                                                                           */
/* The decisions are pure functions in src/net/axl-ssh-penalty.h, pinned     */
/* here with exact values, the way test_ssh_reap_reason pins the login       */
/* grace. The model is OpenSSH 9.9p1's, MEASURED against a throwaway sshd:   */
/* deferred until remaining exceeds min_sec, then enforced until it has      */
/* burned down to nothing.                                                   */
/* ------------------------------------------------------------------------ */

static AxlSshPenalties
test_pen_cfg(uint32_t authfail, uint32_t noauth, uint32_t grace, uint32_t min,
             uint32_t max)
{
    AxlSshPenalties p = { authfail, noauth, grace, min, max };
    return p;
}

/* Source number @a i as an address, 198.51.<hi>.<lo>: two octets, so a
   table of 256 or more distinct sources can be filled. */
static void
test_pen_addr(uint8_t out[4], unsigned i)
{
    out[0] = 198;
    out[1] = 51;
    out[2] = (uint8_t)(i >> 8);
    out[3] = (uint8_t)i;
}

/* A table enabled with @a p, no exempt range, no source. */
static void
test_pen_table(AxlSshPenaltyTable *t, const AxlSshPenalties *p)
{
    axl_ssh_penalty_init(t);
    axl_ssh_penalty_configure(t, p);
}

/* 1 when @a s exempts a.b.c.d, 0 when it does not, -1 when there is no
   table to ask -- so neither a "yes" nor a "no" assertion can pass by
   default. */
static int
test_pen_exempt(const AxlSshServer *s, uint8_t a, uint8_t b, uint8_t c,
                uint8_t d)
{
    const AxlSshPenaltyTable *t = axl_ssh_server_penalty_table(s);
    const uint8_t addr[4] = { a, b, c, d };
    if (t == NULL) {
        return -1;
    }
    return axl_ssh_penalty_is_exempt(t, addr) ? 1 : 0;
}

/* The ladder, rules 1 to 4, in the header's fixed order.

   Falsifier for the forgery sabotage: drop `|| o->forged` from rule 3 in
   axl_ssh_penalty_classify and "a forged signature accrues authfail" fails
   -- the connection sent no USERAUTH_FAILURE, so nothing else marks it. */
static void
test_ssh_penalty_classify(void)
{
    AxlSshConnOutcome all      = { true, true, true, true };
    AxlSshConnOutcome typo     = { .authenticated = true, .refused = true };
    AxlSshConnOutcome both     = { .grace_expired = true, .refused = true };
    AxlSshConnOutcome refused  = { .refused = true };
    AxlSshConnOutcome forged   = { .forged = true };
    AxlSshConnOutcome reaped   = { .grace_expired = true };
    AxlSshConnOutcome nothing  = { false, false, false, false };

    test_check(axl_ssh_penalty_classify(&all) == AXL_SSH_PENALTY_NONE,
               "penalty ladder: rule 1 first -- authenticated accrues nothing"
               " whatever else it did");
    test_check(axl_ssh_penalty_classify(&typo) == AXL_SSH_PENALTY_NONE,
               "penalty ladder: a wrong password then success accrues nothing");
    test_check(axl_ssh_penalty_classify(&both)
                   == AXL_SSH_PENALTY_GRACE_EXCEEDED,
               "penalty ladder: reaped AND refused is grace-exceeded (rule 2"
               " before rule 3)");
    test_check(axl_ssh_penalty_classify(&refused) == AXL_SSH_PENALTY_AUTHFAIL,
               "penalty ladder: a refused attempt (password or publickey)"
               " accrues authfail");
    test_check(axl_ssh_penalty_classify(&forged) == AXL_SSH_PENALTY_AUTHFAIL,
               "penalty ladder: a forged signature accrues authfail, though"
               " no FAILURE was sent");
    test_check(axl_ssh_penalty_classify(&reaped)
                   == AXL_SSH_PENALTY_GRACE_EXCEEDED,
               "penalty ladder: a reaped connection accrues grace-exceeded");
    test_check(axl_ssh_penalty_classify(&nothing) == AXL_SSH_PENALTY_NOAUTH,
               "penalty ladder: no real attempt (none probe, scan, protocol"
               " error) accrues noauth");
    test_check(axl_ssh_penalty_classify(NULL) == AXL_SSH_PENALTY_NOAUTH,
               "penalty ladder: NULL is noauth");
    AxlSshConnOutcome fault = { .grace_expired = true, .refused = true,
                                .server_fault = true };
    AxlSshConnOutcome fault_only = { .server_fault = true };
    test_check(axl_ssh_penalty_classify(&fault) == AXL_SSH_PENALTY_NONE,
               "penalty ladder: a connection the SERVER failed accrues nothing,"
               " whatever else it shows (ruling P6)");
    test_check(axl_ssh_penalty_classify(&fault_only) == AXL_SSH_PENALTY_NONE,
               "penalty ladder: a server fault alone is not noauth");

    /* Distinct values, so a swapped mapping cannot pass. */
    AxlSshPenalties p = test_pen_cfg(7, 3, 11, 20, 60);
    test_check(axl_ssh_penalty_accrual_sec(&p, AXL_SSH_PENALTY_AUTHFAIL) == 7,
               "penalty accrual: authfail maps to authfail_sec");
    test_check(axl_ssh_penalty_accrual_sec(&p, AXL_SSH_PENALTY_NOAUTH) == 3,
               "penalty accrual: noauth maps to noauth_sec");
    test_check(axl_ssh_penalty_accrual_sec(&p, AXL_SSH_PENALTY_GRACE_EXCEEDED)
                   == 11,
               "penalty accrual: grace-exceeded maps to grace_exceeded_sec");
    test_check(axl_ssh_penalty_accrual_sec(&p, AXL_SSH_PENALTY_NONE) == 0,
               "penalty accrual: none accrues 0");

    /* The order is causal, not sorted by size. */
    AxlSshPenalties inv = test_pen_cfg(2, 9, 11, 20, 60);
    test_check(axl_ssh_penalty_accrual_sec(
                   &inv, axl_ssh_penalty_classify(&refused)) == 2,
               "penalty ladder: noauth_sec above authfail_sec still charges a"
               " refused connection authfail_sec");
}

/* Expiry, and the refusal predicate at its exact boundaries. */
static void
test_ssh_penalty_expiry(void)
{
    test_check(axl_ssh_penalty_remaining_ms(6000, 1000) == 5000,
               "penalty remaining: 5000 ms before expiry");
    test_check(axl_ssh_penalty_remaining_ms(6000, 6000) == 0,
               "penalty remaining: 0 at expiry");
    test_check(axl_ssh_penalty_remaining_ms(6000, 9000) == 0,
               "penalty remaining: 0 past expiry, never wrapped");

    AxlSshPenaltySource active   = { .expiry_ms = 6000, .used = true,
                                     .active = true };
    AxlSshPenaltySource deferred = { .expiry_ms = 60000, .used = true,
                                     .active = false };
    test_check(axl_ssh_penalty_refuses(&active, 5999),
               "penalty refuses: an active penalty is refused 1 ms above zero");
    test_check(!axl_ssh_penalty_refuses(&active, 6000),
               "penalty refuses: an active penalty is allowed AT zero");
    test_check(!axl_ssh_penalty_refuses(&deferred, 1000),
               "penalty refuses: a deferred penalty refuses nothing, however"
               " much remains");
    test_check(!axl_ssh_penalty_refuses(NULL, 0),
               "penalty refuses: NULL refuses nothing");
}

/* Deferral, activation and hysteresis, on OpenSSH's defaults and with no
   time passing between accruals -- the measured sequence at its purest.

   Falsifier for the OLD model (refuse only while remaining > min_sec):
   "still refused BELOW min_sec" fails -- 10 s remain and min_sec is 15. */
static void
test_ssh_penalty_activation(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p      = test_pen_cfg(5, 1, 10, 15, 600);
    const uint8_t      src[4] = { 192, 0, 2, 7 };
    bool               act    = true;
    uint64_t           rem    = 0;

    test_pen_table(&t, &p);
    test_check(axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_INSERTED
                   && !act && rem == 5000,
               "penalty: the first accrual is inserted, 5000 ms, deferred");
    test_check(axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && !act && rem == 10000,
               "penalty: the second accrual updates in place, 10000 ms,"
               " deferred");
    test_check(axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && !act && rem == 15000,
               "penalty: exactly min_sec (15000 ms) does not activate");
    test_check(!axl_ssh_penalty_check(&t, src, 1000, NULL),
               "penalty: a source at exactly min_sec is not refused");
    test_check(axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && act && rem == 20000,
               "penalty: one accrual past min_sec activates (20000 ms)");
    test_check(axl_ssh_penalty_check(&t, src, 1000, NULL),
               "penalty: an active source is refused");

    /* Enforced until expiry: the whole penalty, not the part above min. */
    test_check(axl_ssh_penalty_check(&t, src, 11000, NULL),
               "penalty: still refused BELOW min_sec (10000 ms left) -- the"
               " whole penalty, not the part above min");
    test_check(axl_ssh_penalty_check(&t, src, 20999, NULL),
               "penalty: still refused 1 ms before expiry");
    test_check(!axl_ssh_penalty_check(&t, src, 21000, NULL),
               "penalty: allowed at expiry");

    /* After expiry the next accrual is deferred again. */
    test_check(axl_ssh_penalty_record(&t, src, 21000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && !act && rem == 5000,
               "penalty: the first accrual after expiry starts from 0, 5000 ms,"
               " deferred");
    test_check(!axl_ssh_penalty_check(&t, src, 21000, NULL),
               "penalty: one more failed connection after expiry is NOT"
               " refused");
    axl_ssh_penalty_record(&t, src, 21000, AXL_SSH_PENALTY_AUTHFAIL,
                           NULL, NULL);
    axl_ssh_penalty_record(&t, src, 21000, AXL_SSH_PENALTY_AUTHFAIL,
                           NULL, NULL);
    test_check(!axl_ssh_penalty_check(&t, src, 21000, NULL),
               "penalty: 15000 ms after expiry is still deferred");
    test_check(axl_ssh_penalty_record(&t, src, 21000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && act && rem == 20000,
               "penalty: re-activates only past min_sec again");

    /* A connection that was up before activation can end after it. */
    test_check(axl_ssh_penalty_record(&t, src, 21000, AXL_SSH_PENALTY_AUTHFAIL,
                                      &act, &rem) == AXL_SSH_PENALTY_UPDATED
                   && !act && rem == 25000,
               "penalty: an accrual on an active penalty extends it and is not"
               " a second activation");
    const AxlSshPenaltySource *e = axl_ssh_penalty_find(&t, src);
    test_check(e != NULL && e->active && e->expiry_ms == 46000,
               "penalty: the extended entry is active and expires at 46000");
}

/* The ceiling, at a small max and at the largest settable value. */
static void
test_ssh_penalty_ceiling(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p      = test_pen_cfg(5, 1, 10, 15, 20);
    const uint8_t      src[4] = { 192, 0, 2, 9 };
    uint64_t           rem    = 0;

    test_pen_table(&t, &p);
    for (unsigned i = 0; i < 4; i++) {
        axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, &rem);
    }
    test_check(rem == 20000, "penalty ceiling: four accruals reach exactly"
                             " max_sec (20000 ms)");
    axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL, NULL,
                           &rem);
    test_check(rem == 20000, "penalty ceiling: a fifth is capped at max_sec,"
                             " not 25000");

    AxlSshPenalties big = test_pen_cfg(AXL_SSH_PENALTY_FIELD_MAX_SEC, 1, 10, 15,
                                       AXL_SSH_PENALTY_FIELD_MAX_SEC);
    test_pen_table(&t, &big);
    axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL, NULL,
                           NULL);
    axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL, NULL,
                           &rem);
    test_check(rem == 86400000ull,
               "penalty ceiling: at the largest settable value, capped exactly"
               " (86400000 ms), no overflow");
}

/* Disabled, zero accruals, and the exempt list -- each records NOTHING. */
static void
test_ssh_penalty_scope(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p       = test_pen_cfg(5, 0, 10, 15, 600);
    const uint8_t      src[4]  = { 192, 0, 2, 7 };
    const uint8_t      lan[4]  = { 10, 1, 2, 3 };
    const uint8_t      next[4] = { 11, 0, 0, 1 };
    AxlSshPenaltyRange r;

    test_pen_table(&t, &p);
    test_check(axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_NOAUTH,
                                      NULL, NULL) == AXL_SSH_PENALTY_SKIPPED
                   && axl_ssh_penalty_find(&t, src) == NULL,
               "penalty scope: a zero accrual records nothing and takes no"
               " slot");

    for (unsigned i = 0; i < 4; i++) {
        axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
    }
    axl_ssh_penalty_configure(&t, NULL);
    test_check(!axl_ssh_penalty_check(&t, src, 1000, NULL),
               "penalty scope: a disabled table refuses nothing, even a source"
               " it was refusing");
    test_check(axl_ssh_penalty_record(&t, next, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_SKIPPED
                   && axl_ssh_penalty_find(&t, next) == NULL,
               "penalty scope: a disabled table records nothing");

    test_pen_table(&t, &p);
    test_check(axl_ssh_penalty_parse_range("10.0.0.0/8", &r) == AXL_OK,
               "penalty scope: 10.0.0.0/8 parses");
    axl_ssh_penalty_set_exempt(&t, &r, 1);
    bool all_skipped = true;
    for (unsigned i = 0; i < 4; i++) {
        all_skipped = all_skipped
                      && axl_ssh_penalty_record(&t, lan, 1000,
                                                AXL_SSH_PENALTY_AUTHFAIL, NULL,
                                                NULL)
                             == AXL_SSH_PENALTY_SKIPPED;
    }
    test_check(all_skipped && axl_ssh_penalty_find(&t, lan) == NULL,
               "penalty scope: an exempt source never accrues and never takes"
               " a slot");
    test_check(!axl_ssh_penalty_check(&t, lan, 1000, NULL),
               "penalty scope: an exempt source is not refused");
    test_check(axl_ssh_penalty_record(&t, next, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_INSERTED,
               "penalty scope: 11.0.0.1, just outside 10.0.0.0/8, accrues");

    /* Exempt is checked BEFORE the table, not only at accrual. */
    test_pen_table(&t, &p);
    for (unsigned i = 0; i < 4; i++) {
        axl_ssh_penalty_record(&t, src, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
    }
    test_check(axl_ssh_penalty_check(&t, src, 1000, NULL),
               "penalty scope: 192.0.2.7 is refused before any exemption");
    test_check(axl_ssh_penalty_parse_range("192.0.2.0/24", &r) == AXL_OK,
               "penalty scope: 192.0.2.0/24 parses");
    axl_ssh_penalty_set_exempt(&t, &r, 1);
    test_check(!axl_ssh_penalty_check(&t, src, 1000, NULL),
               "penalty scope: exempting a refused source's range lifts the"
               " refusal (checked before the table)");
}

/* A full table: update in place, eviction of the least time left, a drop
   when every entry outranks the newcomer, no churn on a tie, and expired
   slots taken first. */
static void
test_ssh_penalty_table_full(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p = test_pen_cfg(5, 1, 10, 100, 1000);
    uint8_t            a[4];
    const uint8_t      big[4]   = { 203, 0, 113, 1 };
    const uint8_t      small[4] = { 203, 0, 113, 2 };
    unsigned           inserted = 0;
    /* Every source is recorded before this instant and none has expired by
       it: source i expires at 1000 + 10 i + 5000, so at `full` it has
       6000 + 10 i - full ms left, and source 1 has the least. */
    const uint64_t     full = 1000u + AXL_SSH_PENALTY_SOURCES * 10u;

    test_pen_table(&t, &p);
    for (unsigned i = 1; i <= AXL_SSH_PENALTY_SOURCES; i++) {
        test_pen_addr(a, i);
        if (axl_ssh_penalty_record(&t, a, 1000 + i * 10u,
                                   AXL_SSH_PENALTY_AUTHFAIL, NULL, NULL)
                == AXL_SSH_PENALTY_INSERTED) {
            inserted++;
        }
    }
    test_check(inserted == AXL_SSH_PENALTY_SOURCES,
               "penalty table: a full table of distinct sources is all"
               " inserted");

    test_pen_addr(a, 5);
    test_check(axl_ssh_penalty_record(&t, a, full, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_UPDATED,
               "penalty table: a tracked source updates in place on a full"
               " table");

    test_check(axl_ssh_penalty_record(&t, big, full,
                                      AXL_SSH_PENALTY_GRACE_EXCEEDED, NULL,
                                      NULL) == AXL_SSH_PENALTY_EVICTED,
               "penalty table: a newcomer holding more (10000 ms) evicts");
    test_pen_addr(a, 1);
    test_check(axl_ssh_penalty_find(&t, a) == NULL
                   && axl_ssh_penalty_find(&t, big) != NULL,
               "penalty table: ...the entry with the least time left"
               " (source 1)");
    unsigned survivors = 0;
    for (unsigned i = 2; i <= AXL_SSH_PENALTY_SOURCES; i++) {
        test_pen_addr(a, i);
        survivors += axl_ssh_penalty_find(&t, a) != NULL ? 1u : 0u;
    }
    test_check(survivors == AXL_SSH_PENALTY_SOURCES - 1,
               "penalty table: ...and only that one");

    test_check(axl_ssh_penalty_record(&t, small, full, AXL_SSH_PENALTY_NOAUTH,
                                      NULL, NULL) == AXL_SSH_PENALTY_DROPPED
                   && axl_ssh_penalty_find(&t, small) == NULL
                   && axl_ssh_penalty_find(&t, big) != NULL,
               "penalty table: a newcomer every entry outranks (1000 ms) is"
               " dropped, and nothing changes");

    /* A tie does not churn: a full table of 5000 ms entries, and one more
       of 5000 ms. */
    test_pen_table(&t, &p);
    for (unsigned i = 1; i <= AXL_SSH_PENALTY_SOURCES; i++) {
        test_pen_addr(a, i);
        axl_ssh_penalty_record(&t, a, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
    }
    test_check(axl_ssh_penalty_record(&t, big, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_DROPPED,
               "penalty table: an equal remaining is dropped, not swapped in");

    /* Expired slots are free, however small the newcomer. */
    test_check(axl_ssh_penalty_record(&t, small, 7000, AXL_SSH_PENALTY_NOAUTH,
                                      NULL, NULL) == AXL_SSH_PENALTY_INSERTED,
               "penalty table: an expired slot is taken before anything is"
               " evicted");
}

/* Ruling P4: retention ranks ACTIVE first, then time left. Every case below
   has an active entry with LESS time left than the deferred entry it must
   outlive, so ranking by raw time left gives the opposite answer.

   Falsifier: rank by raw remaining again (drop the active term from
   penalty_rank in axl-ssh-penalty.c) and the first four checks after the
   precondition fail -- the tie, the deferred eviction, the active-on-arrival
   displacement and the all-active drop. The last check passes either way:
   on an all-active table the two rankings agree. */
static void
test_ssh_penalty_table_ranking(void)
{
    AxlSshPenaltyTable t;
    /* authfail 7: two accruals (14000) stay deferred under min 15, three
       (21000) activate; noauth 10 is a deferred newcomer between the two;
       grace-exceeded 20 is active on arrival. */
    AxlSshPenalties    p = test_pen_cfg(7, 10, 20, 15, 1000);
    uint8_t            a[4];
    const uint8_t      act[4]    = { 203, 0, 113, 9 };
    const uint8_t      least[4]  = { 203, 0, 113, 8 };
    const uint8_t      nd[4]     = { 203, 0, 113, 1 };
    const uint8_t      na[4]     = { 203, 0, 113, 2 };
    bool               activated = false;

    /* One active entry, recorded at 1000: 21000 ms, expiring at 22000 --
       2000 ms left at 20000. */
    test_pen_table(&t, &p);
    for (unsigned i = 0; i < 3; i++) {
        axl_ssh_penalty_record(&t, act, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
    }
    /* One deferred entry at 7000 and the rest of the table at 14000, all
       at 20000. */
    axl_ssh_penalty_record(&t, least, 20000, AXL_SSH_PENALTY_AUTHFAIL,
                           NULL, NULL);
    for (unsigned i = 1; i <= AXL_SSH_PENALTY_SOURCES - 2; i++) {
        test_pen_addr(a, i);
        axl_ssh_penalty_record(&t, a, 20000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
        axl_ssh_penalty_record(&t, a, 20000, AXL_SSH_PENALTY_AUTHFAIL,
                               NULL, NULL);
    }
    test_check(axl_ssh_penalty_check(&t, act, 20000, NULL),
               "penalty rank: the active entry is refusing with 2000 ms left");

    /* A deferred newcomer of 7000 only ties the least deferred entry; one
       of 10000 outranks it -- and nothing deferred outranks the active one. */
    test_check(axl_ssh_penalty_record(&t, nd, 20000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_DROPPED
                   && axl_ssh_penalty_check(&t, act, 20000, NULL),
               "penalty rank: a deferred newcomer tying the least deferred"
               " entry (7000 ms) is dropped; the active one is untouched");
    test_check(axl_ssh_penalty_record(&t, nd, 20000,
                                      AXL_SSH_PENALTY_NOAUTH, NULL, NULL)
                   == AXL_SSH_PENALTY_EVICTED
                   && axl_ssh_penalty_find(&t, least) == NULL
                   && axl_ssh_penalty_check(&t, act, 20000, NULL),
               "penalty rank: a deferred newcomer (10000 ms) evicts the least"
               " deferred entry, never the active one (2000 ms left)");

    /* An active-on-arrival newcomer displaces a deferred entry, not the
       active one with less time left. */
    test_check(axl_ssh_penalty_record(&t, na, 20000,
                                      AXL_SSH_PENALTY_GRACE_EXCEEDED,
                                      &activated, NULL)
                   == AXL_SSH_PENALTY_EVICTED
                   && activated
                   && axl_ssh_penalty_check(&t, na, 20000, NULL)
                   && axl_ssh_penalty_check(&t, act, 20000, NULL),
               "penalty rank: an active-on-arrival newcomer displaces a"
               " deferred entry and the active one survives");

    /* An all-active full table. Source i records three accruals at
       1000 + 10 i, so at 20000 it has 2000 + 10 i ms left. */
    test_pen_table(&t, &p);
    for (unsigned i = 1; i <= AXL_SSH_PENALTY_SOURCES; i++) {
        test_pen_addr(a, i);
        for (unsigned k = 0; k < 3; k++) {
            axl_ssh_penalty_record(&t, a, 1000 + i * 10u,
                                   AXL_SSH_PENALTY_AUTHFAIL, NULL, NULL);
        }
    }
    test_check(axl_ssh_penalty_record(&t, nd, 20000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_DROPPED
                   && axl_ssh_penalty_find(&t, nd) == NULL,
               "penalty rank: a deferred newcomer (7000 ms) against an"
               " all-active table is dropped, though the least active has"
               " 2010 ms");
    test_pen_addr(a, 1);
    test_check(axl_ssh_penalty_record(&t, na, 20000,
                                      AXL_SSH_PENALTY_GRACE_EXCEEDED, NULL,
                                      NULL) == AXL_SSH_PENALTY_EVICTED
                   && axl_ssh_penalty_find(&t, a) == NULL
                   && axl_ssh_penalty_find(&t, na) != NULL,
               "penalty rank: on an all-active table an active newcomer evicts"
               " the least-remaining active (source 1)");
}

/* Ruling P5: 256 sources. A LITERAL here, on purpose: the table-full and
   ranking tests above are written against AXL_SSH_PENALTY_SOURCES and would
   pass at any size, so this is the one place the number itself is pinned. */
static void
test_ssh_penalty_capacity(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p        = test_pen_cfg(5, 1, 10, 100, 1000);
    uint8_t            a[4];
    unsigned           inserted = 0;

    test_pen_table(&t, &p);
    for (unsigned i = 1; i <= 256; i++) {
        test_pen_addr(a, i);
        if (axl_ssh_penalty_record(&t, a, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                   NULL, NULL) == AXL_SSH_PENALTY_INSERTED) {
            inserted++;
        }
    }
    test_check(inserted == 256,
               "penalty capacity: 256 distinct sources are all tracked"
               " (ruling P5)");
    test_pen_addr(a, 257);
    test_check(axl_ssh_penalty_record(&t, a, 1000, AXL_SSH_PENALTY_AUTHFAIL,
                                      NULL, NULL) == AXL_SSH_PENALTY_DROPPED
                   && axl_ssh_penalty_find(&t, a) == NULL,
               "penalty capacity: the 257th, only tying the rest, is the first"
               " source not tracked");
}

/* Ruling P5's visibility half: the first dropped accrual in each max_sec
   window is reported at INFO, and no more than that. */
static void
test_ssh_penalty_drop_notice(void)
{
    AxlSshPenaltyTable t;
    AxlSshPenalties    p = test_pen_cfg(5, 1, 10, 15, 600);

    test_pen_table(&t, &p);
    test_check(axl_ssh_penalty_drop_notice(&t, 1000),
               "drop notice: the first drop is reported at INFO");
    test_check(!axl_ssh_penalty_drop_notice(&t, 1001),
               "drop notice: the next, 1 ms later, is not");
    test_check(!axl_ssh_penalty_drop_notice(&t, 600999),
               "drop notice: nor one max_sec less 1 ms after the first");
    test_check(axl_ssh_penalty_drop_notice(&t, 601000),
               "drop notice: one max_sec after the first, one more is");
    test_check(!axl_ssh_penalty_drop_notice(&t, 601001),
               "drop notice: and the window restarts from that one");
}

/* The exempt entry parser: axl_ipv4_parse_cidr's refusals, plus AxlSsh's
   own two -- host bits set, and /0 as POLICY. */
static bool
test_pen_range_is(const AxlSshPenaltyRange *r, const uint8_t a[4],
                  const uint8_t m[4])
{
    return axl_memcmp(r->addr, a, 4) == 0 && axl_memcmp(r->mask, m, 4) == 0;
}

static void
test_ssh_penalty_parse_range(void)
{
    AxlSshPenaltyRange r;
    static const uint8_t net10[4]   = { 10, 0, 0, 0 };
    static const uint8_t mask8[4]   = { 255, 0, 0, 0 };
    static const uint8_t host7[4]   = { 192, 0, 2, 7 };
    static const uint8_t mask32[4]  = { 255, 255, 255, 255 };
    static const uint8_t zero[4]    = { 0, 0, 0, 0 };
    static const uint8_t mask1[4]   = { 128, 0, 0, 0 };

    test_check(axl_ssh_penalty_parse_range("10.0.0.0/8", &r) == AXL_OK
                   && test_pen_range_is(&r, net10, mask8),
               "exempt parse: 10.0.0.0/8");
    axl_memset(&r, 0, sizeof r);
    test_check(axl_ssh_penalty_parse_range("192.0.2.7", &r) == AXL_OK
                   && test_pen_range_is(&r, host7, mask32),
               "exempt parse: a bare address means /32");
    axl_memset(&r, 0, sizeof r);
    test_check(axl_ssh_penalty_parse_range("192.0.2.7/32", &r) == AXL_OK
                   && test_pen_range_is(&r, host7, mask32),
               "exempt parse: 192.0.2.7/32");
    test_check(axl_ssh_penalty_parse_range("0.0.0.0/1", &r) == AXL_OK
                   && test_pen_range_is(&r, zero, mask1),
               "exempt parse: /1, the widest prefix accepted");
    test_check(axl_ssh_penalty_parse_range("192.0.2.0/8", &r) == AXL_INVALID,
               "exempt parse: host bits set below the prefix are refused");
    test_check(axl_ssh_penalty_parse_range("10.1.2.3/8", &r) == AXL_INVALID,
               "exempt parse: 10.1.2.3/8 (meant 10.0.0.0/8) is refused");
    test_check(axl_ssh_penalty_parse_range("0.0.0.0/0", &r) == AXL_INVALID,
               "exempt parse: 0.0.0.0/0 -- no host bits, refused as policy");
    test_check(axl_ssh_penalty_parse_range("10.0.0.0/33", &r) == AXL_INVALID,
               "exempt parse: /33 is refused");
    test_check(axl_ssh_penalty_parse_range("10.0.0.0/", &r) == AXL_INVALID,
               "exempt parse: an empty prefix is refused");
    test_check(axl_ssh_penalty_parse_range("10.0.0.0/8x", &r) == AXL_INVALID,
               "exempt parse: trailing garbage is refused");
    test_check(axl_ssh_penalty_parse_range("10.0.0", &r) == AXL_INVALID,
               "exempt parse: three octets are refused");
    test_check(axl_ssh_penalty_parse_range("10.0.0.256", &r) == AXL_INVALID,
               "exempt parse: an octet over 255 is refused");
    test_check(axl_ssh_penalty_parse_range("", &r) == AXL_INVALID,
               "exempt parse: an empty string is refused");
    test_check(axl_ssh_penalty_parse_range(NULL, &r) == AXL_INVALID,
               "exempt parse: NULL is refused");
    /* Task P review M-4: an octet of 2^32 + 10 used to wrap to 10 inside
       axl_ipv4_parse, so this installed as 10.0.0.0/32. */
    test_check(axl_ssh_penalty_parse_range("4294967306.0.0.0", &r)
                   == AXL_INVALID,
               "exempt parse: a wrapping octet (2^32 + 10) is refused, not"
               " read as 10.0.0.0");
    test_check(axl_ssh_penalty_parse_range("4294967306.0.0.0/8", &r)
                   == AXL_INVALID,
               "exempt parse: ...and with a prefix");
}

/* axl_ssh_penalty_range_check() is the setter's rule without a server: one
   case per refusal (and per acceptance) through BOTH entry points, so the
   two can never disagree about an entry. */
static void
test_ssh_penalty_range_check(void)
{
    static const struct {
        const char *range;
        int         want;
    } cases[] = {
        { "10.0.0.0/8",         AXL_OK },
        { "192.0.2.7",          AXL_OK },
        { "0.0.0.0/1",          AXL_OK },
        { "10.1.2.3/8",         AXL_INVALID },  /* bits below the prefix */
        { "0.0.0.0/0",          AXL_INVALID },  /* /0, policy */
        { "10.0.0.0/33",        AXL_INVALID },  /* prefix over 32 */
        { "10.0.0.0/",          AXL_INVALID },  /* empty prefix */
        { "10.0.0.0/8x",        AXL_INVALID },  /* trailing garbage */
        { "10.0.0",             AXL_INVALID },  /* does not parse */
        { "",                   AXL_INVALID },
    };
    const size_t n = AXL_ARRAY_SIZE(cases);

    test_check(axl_ssh_penalty_range_check(NULL) == AXL_INVALID,
               "range check: NULL is refused");

    AxlLoop *loop = axl_loop_new();
    AxlSshServer *s = loop != NULL ? axl_ssh_server_new(loop, 2261) : NULL;
    test_check(s != NULL, "range check: server created");
    for (size_t i = 0; i < n; i++) {
        char name[96];
        axl_snprintf(name, sizeof name, "range check: \"%s\" -> %s",
                     cases[i].range,
                     cases[i].want == AXL_OK ? "AXL_OK" : "AXL_INVALID");
        test_check(axl_ssh_penalty_range_check(cases[i].range) == cases[i].want,
                   name);
        if (s == NULL) {
            test_skip_n(1, "no server (Ed25519 provider or RNG unavailable)");
            continue;
        }
        const char *one[1] = { cases[i].range };
        axl_snprintf(name, sizeof name,
                     "range check: set_penalty_exempt agrees on \"%s\"",
                     cases[i].range);
        test_check(axl_ssh_server_set_penalty_exempt(s, one, 1) == cases[i].want,
                   name);
    }
    axl_ssh_server_free(s);
    axl_loop_free(loop);
}

/* The public API: defaults, validation, the exempt setter's all-or-nothing
   and bound, and the independence of the two setters. */
static void
test_ssh_penalty_api(void)
{
    AxlSshPenalties d;
    test_check(axl_ssh_penalties_default(NULL) == AXL_INVALID,
               "penalty api: default refuses NULL");
    test_check(axl_ssh_penalties_default(&d) == AXL_OK
                   && d.authfail_sec == 5 && d.noauth_sec == 1
                   && d.grace_exceeded_sec == 10 && d.min_sec == 15
                   && d.max_sec == 600,
               "penalty api: default fills OpenSSH 9.9p1's numbers exactly");

    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "penalty api: loop");
    if (loop == NULL) {
        test_skip_n(29, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2260);
    test_check(s != NULL, "penalty api: server created");
    if (s == NULL) {
        test_skip_n(28, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }

    const AxlSshPenaltyTable *t = axl_ssh_server_penalty_table(s);
    test_check(t != NULL && t->enabled && t->nexempt == 0
                   && t->cfg.authfail_sec == 5 && t->cfg.noauth_sec == 1
                   && t->cfg.grace_exceeded_sec == 10 && t->cfg.min_sec == 15
                   && t->cfg.max_sec == 600,
               "penalty api: a new server has penalties ON at the defaults,"
               " exempting nothing");

    /* set_penalties validation. */
    AxlSshPenalties p = test_pen_cfg(5, 1, 10, 15, 600);
    AxlSshPenalties bad;
    test_check(axl_ssh_server_set_penalties(NULL, &p) == AXL_INVALID,
               "penalty api: set refuses a NULL server");
    bad = p;
    bad.authfail_sec = AXL_SSH_PENALTY_FIELD_MAX_SEC + 1;
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_INVALID,
               "penalty api: a field one over FIELD_MAX is refused");
    bad = test_pen_cfg(5, 1, 10, 15, AXL_SSH_PENALTY_FIELD_MAX_SEC);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_OK,
               "penalty api: max_sec exactly FIELD_MAX is accepted");
    bad = test_pen_cfg(5, 1, 10, 20, 20);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_INVALID,
               "penalty api: min_sec == max_sec is refused (can never"
               " activate)");
    bad = test_pen_cfg(5, 1, 10, 21, 20);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_INVALID,
               "penalty api: min_sec above max_sec is refused");
    bad = test_pen_cfg(5, 1, 10, 19, 20);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_OK,
               "penalty api: min_sec one below max_sec is accepted");
    bad = test_pen_cfg(0, 0, 0, 15, 600);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_INVALID,
               "penalty api: three zero accruals are refused");
    bad = test_pen_cfg(0, 0, 10, 15, 600);
    test_check(axl_ssh_server_set_penalties(s, &bad) == AXL_OK,
               "penalty api: one non-zero accrual is enough");
    test_check(axl_ssh_server_set_penalties(s, NULL) == AXL_OK
                   && t != NULL && !t->enabled,
               "penalty api: NULL turns penalties off");

    /* set_penalty_exempt validation, bound and all-or-nothing. */
    static const char *const one[]  = { "10.0.0.0/8" };
    static const char *const nine[] = { "10.0.0.1", "10.0.0.2", "10.0.0.3",
                                        "10.0.0.4", "10.0.0.5", "10.0.0.6",
                                        "10.0.0.7", "10.0.0.8", "10.0.0.9" };
    static const char *const half[] = { "192.168.0.0/16", "10.1.2.3/8" };
    static const char *const holey[] = { "192.168.0.0/16", NULL };
    static const char *const every[] = { "0.0.0.0/0" };
    test_check(axl_ssh_server_set_penalty_exempt(NULL, one, 1) == AXL_INVALID,
               "exempt api: refuses a NULL server");
    test_check(axl_ssh_server_set_penalty_exempt(s, NULL, 1) == AXL_INVALID,
               "exempt api: refuses NULL ranges with a count");
    test_check(axl_ssh_server_set_penalty_exempt(s, one, 1) == AXL_OK
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1,
               "exempt api: 10.0.0.0/8 installed; 10.9.9.9 is exempt");
    test_check(axl_ssh_server_set_penalty_exempt(s, half, 2) == AXL_INVALID
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1
                   && test_pen_exempt(s, 192, 168, 1, 1) == 0,
               "exempt api: one bad entry changes nothing (all or nothing)");
    test_check(axl_ssh_server_set_penalty_exempt(s, holey, 2) == AXL_INVALID
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1,
               "exempt api: a NULL entry is refused, list unchanged");
    test_check(axl_ssh_server_set_penalty_exempt(s, every, 1) == AXL_INVALID
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1,
               "exempt api: a well-formed /0 is refused as policy");
    test_check(axl_ssh_server_set_penalty_exempt(s, nine, 9)
                   == AXL_NO_RESOURCES
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1,
               "exempt api: a ninth entry is AXL_NO_RESOURCES, list unchanged");
    test_check(axl_ssh_server_set_penalty_exempt(s, nine, 8) == AXL_OK
                   && test_pen_exempt(s, 10, 0, 0, 8) == 1
                   && test_pen_exempt(s, 10, 0, 0, 9) == 0,
               "exempt api: exactly eight are accepted, replacing the list");

    /* Independence, both orders, and a disable/enable round trip. */
    test_check(axl_ssh_server_set_penalty_exempt(s, one, 1) == AXL_OK
                   && axl_ssh_server_set_penalties(s, &p) == AXL_OK
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1
                   && t != NULL && t->enabled,
               "independence: set_penalties after set_penalty_exempt keeps"
               " the list");
    test_check(axl_ssh_server_set_penalties(s, NULL) == AXL_OK
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1,
               "independence: set_penalties(NULL) keeps the list");
    test_check(axl_ssh_server_set_penalties(s, &p) == AXL_OK
                   && test_pen_exempt(s, 10, 9, 9, 9) == 1
                   && t != NULL && t->enabled,
               "independence: re-enabling keeps the list");
    test_check(axl_ssh_server_set_penalty_exempt(s, NULL, 0) == AXL_OK
                   && test_pen_exempt(s, 10, 9, 9, 9) == 0
                   && t != NULL && t->enabled,
               "independence: count 0 clears the list and leaves penalties"
               " on");

    AxlSshServer *s2 = axl_ssh_server_new(loop, 2261);
    test_check(s2 != NULL, "penalty api: second server created");
    if (s2 != NULL) {
        AxlSshPenalties q = test_pen_cfg(9, 2, 12, 30, 90);
        const AxlSshPenaltyTable *t2 = axl_ssh_server_penalty_table(s2);
        test_check(axl_ssh_server_set_penalties(s2, &q) == AXL_OK
                       && axl_ssh_server_set_penalty_exempt(s2, one, 1)
                              == AXL_OK
                       && test_pen_exempt(s2, 10, 9, 9, 9) == 1
                       && t2 != NULL && t2->enabled
                       && t2->cfg.authfail_sec == 9 && t2->cfg.min_sec == 30,
                   "independence: set_penalty_exempt after set_penalties"
                   " keeps the settings");
        axl_ssh_server_free(s2);
    } else {
        test_skip_n(1, "no second server");
    }

    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_penalties(s, &p) == AXL_ERR,
               "penalty api: set_penalties refused on a freed server");
    test_check(axl_ssh_server_set_penalty_exempt(s, one, 1) == AXL_ERR,
               "exempt api: refused on a freed server");

    /* Already listening: a real bind, as test_ssh_login_grace_api does. */
    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2262);
    char         *key  = live != NULL ? test_ssh_gen_authkey_line() : NULL;
    bool ready = key != NULL
                 && axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_penalties(live, &p) == AXL_ERR,
                   "penalty api: set_penalties refused once listening");
        test_check(axl_ssh_server_set_penalty_exempt(live, one, 1) == AXL_ERR,
                   "exempt api: refused once listening");
    } else {
        test_skip_n(2, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }
    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

/* ---- RFC 4254 channel message codec ---------------------------------- */

/* First prefix length of @a msg (shorter than the whole, from @a from) that a
   parser wrongly accepts, or -1. A parser that skips ONE length check accepts
   the prefix that check guards, and this names it by failing its assertion. */
typedef int (*ChanPrefixFn)(const uint8_t *p, size_t len);

static int
chan_first_accepted_prefix(ChanPrefixFn fn, const uint8_t *msg, size_t len,
                           size_t from)
{
    for (size_t n = from; n < len; n++) {
        if (fn(msg, n) == AXL_OK) {
            return (int)n;
        }
    }
    return -1;
}

/* Exact bytes of a builder's output. */
static bool
chan_bytes_eq(AxlString *b, const uint8_t *want, size_t n)
{
    return axl_string_len(b) == n
           && axl_memcmp(axl_string_data(b), want, n) == 0;
}

static const uint8_t CHAN_OPEN_MSG[] = {
    90,                                     /* CHANNEL_OPEN */
    0, 0, 0, 7, 's','e','s','s','i','o','n',/* type */
    0, 0, 0, 5,                             /* sender */
    0, 0, 0x80, 0,                          /* window 32768 */
    0, 0, 0x40, 0,                          /* max packet 16384 */
};

static int
chan_open_fn(const uint8_t *p, size_t len)
{
    AxlSshChanOpen o;
    return axl_ssh_chan_open_parse(p, len, &o);
}

static void
test_ssh_chan_open_parse(void)
{
    AxlSshChanOpen o;
    axl_memset(&o, 0, sizeof o);
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, sizeof CHAN_OPEN_MSG, &o)
                   == AXL_OK, "chan open: valid vector parses");
    test_check(o.type_len == 7 && o.type == CHAN_OPEN_MSG + 5
                   && axl_memcmp(o.type, "session", 7) == 0,
               "chan open: type is 'session', pointing into the input");
    test_check(o.sender == 5, "chan open: sender channel");
    test_check(o.window == 0x8000, "chan open: window");
    test_check(o.max_packet == 0x4000, "chan open: max packet");
    test_check(chan_first_accepted_prefix(chan_open_fn, CHAN_OPEN_MSG,
                                          sizeof CHAN_OPEN_MSG, 0) == -1,
               "chan open: every truncation is refused");
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, sizeof CHAN_OPEN_MSG - 1,
                                       &o) == AXL_ERR,
               "chan open: truncated max_packet is refused");
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, 5 + 7 + 4 + 4, &o)
                   == AXL_ERR,
               "chan open: missing max_packet is refused");
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, 5 + 7 + 4, &o) == AXL_ERR,
               "chan open: missing window is refused");
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, 5 + 7, &o) == AXL_ERR,
               "chan open: missing sender is refused");
    test_check(o.rest == CHAN_OPEN_MSG + sizeof CHAN_OPEN_MSG
                   && o.rest_len == 0,
               "chan open: a session open has an empty rest, at the end");
    /* NOT refused: what follows max packet is type-specific (x11, port
       forwarding), and a type the server refuses must draw OPEN_FAILURE,
       not end the connection. The caller checks a type it accepts. */
    uint8_t t[sizeof CHAN_OPEN_MSG + 1];
    axl_memcpy(t, CHAN_OPEN_MSG, sizeof CHAN_OPEN_MSG);
    t[sizeof CHAN_OPEN_MSG] = 0;
    test_check(axl_ssh_chan_open_parse(t, sizeof t, &o) == AXL_OK
                   && o.rest == t + sizeof CHAN_OPEN_MSG && o.rest_len == 1,
               "chan open: trailing type-specific data is accepted, as rest");
    /* RFC 4254 section 6.3.2's x11 open, originator address and port. */
    static const uint8_t X11[] = {
        90, 0, 0, 0, 3, 'x', '1', '1', 0, 0, 0, 1, 0, 0, 0x80, 0,
        0, 0, 0x80, 0, 0, 0, 0, 9, '1', '2', '7', '.', '0', '.', '0', '.',
        '1', 0, 0, 0x17, 0x7a,
    };
    test_check(axl_ssh_chan_open_parse(X11, sizeof X11, &o) == AXL_OK
                   && o.type_len == 3 && axl_memcmp(o.type, "x11", 3) == 0
                   && o.sender == 1 && o.rest == X11 + 20 && o.rest_len == 17,
               "chan open: an RFC 4254 x11 open parses, its originator as rest");
    axl_memcpy(t, CHAN_OPEN_MSG, sizeof CHAN_OPEN_MSG);
    t[0] = 91;
    test_check(axl_ssh_chan_open_parse(t, sizeof CHAN_OPEN_MSG, &o) == AXL_ERR,
               "chan open: a type other than 90 is refused");
    test_check(axl_ssh_chan_open_parse(CHAN_OPEN_MSG, sizeof CHAN_OPEN_MSG, NULL)
                   == AXL_ERR, "chan open: NULL out is refused");
    test_check(axl_ssh_chan_open_parse(NULL, 4, &o) == AXL_ERR,
               "chan open: NULL input is refused");
}

static const uint8_t CHAN_REQ_MSG[] = {
    98,                                     /* CHANNEL_REQUEST */
    0, 0, 0, 9,                             /* recipient */
    0, 0, 0, 4, 'e','x','e','c',            /* request name */
    1,                                      /* want_reply */
    0, 0, 0, 2, 'l','s',                    /* request-specific: string "ls" */
};

static int
chan_req_fn(const uint8_t *p, size_t len)
{
    AxlSshChanReq r;
    return axl_ssh_chan_req_parse(p, len, &r);
}

static void
test_ssh_chan_req_parse(void)
{
    AxlSshChanReq r;
    axl_memset(&r, 0, sizeof r);
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, sizeof CHAN_REQ_MSG, &r)
                   == AXL_OK, "chan req: valid vector parses");
    test_check(r.recipient == 9, "chan req: recipient");
    test_check(r.name_len == 4 && axl_memcmp(r.name, "exec", 4) == 0,
               "chan req: name");
    test_check(r.want_reply, "chan req: want_reply TRUE");
    test_check(r.rest == CHAN_REQ_MSG + 14 && r.rest_len == 6,
               "chan req: rest is the request-specific bytes, unread");

    /* Header only: rest is empty, not an error (e.g. "shell"). */
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, 14, &r) == AXL_OK
                   && r.rest_len == 0,
               "chan req: no request-specific data parses with empty rest");
    test_check(chan_first_accepted_prefix(chan_req_fn, CHAN_REQ_MSG, 14, 0)
                   == -1, "chan req: every truncation of the header is refused");
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, 13, &r) == AXL_ERR,
               "chan req: missing want_reply is refused");
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, 12, &r) == AXL_ERR,
               "chan req: truncated name is refused");
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, 5, &r) == AXL_ERR,
               "chan req: missing name is refused");

    uint8_t t[sizeof CHAN_REQ_MSG];
    axl_memcpy(t, CHAN_REQ_MSG, sizeof t);
    t[13] = 2;
    test_check(axl_ssh_chan_req_parse(t, sizeof t, &r) == AXL_OK
                   && r.want_reply,
               "chan req: any non-zero want_reply is TRUE");
    t[13] = 0;
    test_check(axl_ssh_chan_req_parse(t, sizeof t, &r) == AXL_OK
                   && !r.want_reply,
               "chan req: want_reply 0 is FALSE");
    t[0] = 97;
    test_check(axl_ssh_chan_req_parse(t, sizeof t, &r) == AXL_ERR,
               "chan req: a type other than 98 is refused");
    test_check(axl_ssh_chan_req_parse(CHAN_REQ_MSG, sizeof CHAN_REQ_MSG, NULL)
                   == AXL_ERR, "chan req: NULL out is refused");
    test_check(axl_ssh_chan_req_parse(NULL, 14, &r) == AXL_ERR,
               "chan req: NULL input is refused");
}

static const uint8_t CHAN_PTY_REST[] = {
    0, 0, 0, 5, 'x','t','e','r','m',        /* TERM */
    0, 0, 0, 80,                            /* cols */
    0, 0, 0, 24,                            /* rows */
    0, 0, 3, 0x20,                          /* width px 800 */
    0, 0, 2, 0x58,                          /* height px 600 */
    0, 0, 0, 0,                             /* modes: empty string */
};

static char chan_term[8];
static uint32_t chan_dims[4];

static int
chan_pty_fn(const uint8_t *p, size_t len)
{
    return axl_ssh_chan_pty_parse(p, len, chan_term, sizeof chan_term, chan_dims);
}

static int
chan_winch_fn(const uint8_t *p, size_t len)
{
    return axl_ssh_chan_winch_parse(p, len, chan_dims);
}

static void
test_ssh_chan_pty_parse(void)
{
    char term[8];
    uint32_t d[4] = { 0, 0, 0, 0 };
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, sizeof CHAN_PTY_REST,
                                      term, sizeof term, d) == AXL_OK,
               "chan pty: valid vector parses");
    test_check(axl_strcmp(term, "xterm") == 0, "chan pty: TERM copied");
    test_check(d[0] == 80 && d[1] == 24 && d[2] == 800 && d[3] == 600,
               "chan pty: cols rows wpx hpx");
    test_check(chan_first_accepted_prefix(chan_pty_fn, CHAN_PTY_REST,
                                          sizeof CHAN_PTY_REST, 0) == -1,
               "chan pty: every truncation is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, sizeof CHAN_PTY_REST - 1,
                                      term, sizeof term, d) == AXL_ERR,
               "chan pty: truncated modes length is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, 9 + 16, term, sizeof term,
                                      d) == AXL_ERR,
               "chan pty: missing modes string is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, 9 + 15, term, sizeof term,
                                      d) == AXL_ERR,
               "chan pty: truncated height is refused");

    /* A non-empty modes string (TTY_OP_END only) is accepted and ignored. */
    uint8_t big[sizeof CHAN_PTY_REST + 8];
    axl_memcpy(big, CHAN_PTY_REST, sizeof CHAN_PTY_REST);
    big[sizeof CHAN_PTY_REST - 1] = 5;      /* modes length 5 */
    big[sizeof CHAN_PTY_REST + 0] = 1;      /* VINTR opcode */
    big[sizeof CHAN_PTY_REST + 1] = 0;
    big[sizeof CHAN_PTY_REST + 2] = 0;
    big[sizeof CHAN_PTY_REST + 3] = 0;
    big[sizeof CHAN_PTY_REST + 4] = 3;
    big[sizeof CHAN_PTY_REST + 5] = 0;      /* one byte past: trailing */
    test_check(axl_ssh_chan_pty_parse(big, sizeof CHAN_PTY_REST + 5, term,
                                      sizeof term, d) == AXL_OK
                   && d[0] == 80,
               "chan pty: a non-empty modes string is accepted and ignored");
    test_check(axl_ssh_chan_pty_parse(big, sizeof CHAN_PTY_REST + 4, term,
                                      sizeof term, d) == AXL_ERR,
               "chan pty: modes string running past the end is refused");
    test_check(axl_ssh_chan_pty_parse(big, sizeof CHAN_PTY_REST + 6, term,
                                      sizeof term, d) == AXL_ERR,
               "chan pty: a trailing byte after the modes is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, sizeof CHAN_PTY_REST, NULL,
                                      sizeof term, d) == AXL_ERR,
               "chan pty: NULL term is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, sizeof CHAN_PTY_REST, term,
                                      sizeof term, NULL) == AXL_ERR,
               "chan pty: NULL dims is refused");
    test_check(axl_ssh_chan_pty_parse(CHAN_PTY_REST, sizeof CHAN_PTY_REST, term,
                                      0, d) == AXL_ERR,
               "chan pty: zero term_cap is refused");
    test_check(axl_ssh_chan_pty_parse(NULL, 4, term, sizeof term, d) == AXL_ERR,
               "chan pty: NULL input is refused");

    /* TERM length against term_cap. cap 8 holds 7 characters. */
    uint8_t v[sizeof CHAN_PTY_REST + 3];
    axl_memcpy(v, CHAN_PTY_REST, 4);
    v[3] = 7;
    axl_memcpy(v + 4, "1234567", 7);
    axl_memcpy(v + 11, CHAN_PTY_REST + 9, sizeof CHAN_PTY_REST - 9);
    size_t vn = sizeof CHAN_PTY_REST - 9 + 11;
    test_check(axl_ssh_chan_pty_parse(v, vn, term, sizeof term, d) == AXL_OK
                   && axl_strcmp(term, "1234567") == 0,
               "chan pty: TERM of term_cap - 1 is accepted and NUL-terminated");
    uint8_t w[sizeof CHAN_PTY_REST + 3];
    axl_memcpy(w, CHAN_PTY_REST, 4);
    w[3] = 8;
    axl_memcpy(w + 4, "12345678", 8);
    axl_memcpy(w + 12, CHAN_PTY_REST + 9, sizeof CHAN_PTY_REST - 9);
    test_check(axl_ssh_chan_pty_parse(w, vn + 1, term, sizeof term, d)
                   == AXL_ERR,
               "chan pty: TERM of term_cap is refused");

    /* NUL handling, as sshbuf_get_cstring: inside is a protocol error, a
       single one as the last byte is stripped. */
    uint8_t n1[sizeof CHAN_PTY_REST];
    axl_memcpy(n1, CHAN_PTY_REST, sizeof n1);
    n1[6] = 0;                              /* "x\0erm" */
    test_check(axl_ssh_chan_pty_parse(n1, sizeof n1, term, sizeof term, d)
                   == AXL_ERR, "chan pty: a NUL inside TERM is refused");
    axl_memcpy(n1, CHAN_PTY_REST, sizeof n1);
    n1[4] = 0;                              /* "\0term" */
    test_check(axl_ssh_chan_pty_parse(n1, sizeof n1, term, sizeof term, d)
                   == AXL_ERR, "chan pty: a leading NUL in TERM is refused");
    axl_memcpy(n1, CHAN_PTY_REST, sizeof n1);
    n1[8] = 0;                              /* "xter\0": NUL is the last byte */
    test_check(axl_ssh_chan_pty_parse(n1, sizeof n1, term, sizeof term, d)
                   == AXL_OK && axl_strcmp(term, "xter") == 0,
               "chan pty: one trailing NUL in TERM is accepted and stripped");
    uint8_t n2[sizeof CHAN_PTY_REST];
    axl_memcpy(n2, CHAN_PTY_REST, sizeof n2);
    n2[7] = 0;
    n2[8] = 0;                              /* "xte\0\0": two NULs */
    test_check(axl_ssh_chan_pty_parse(n2, sizeof n2, term, sizeof term, d)
                   == AXL_ERR, "chan pty: two trailing NULs in TERM are refused");
}

static const uint8_t CHAN_WINCH_REST[] = {
    0, 0, 0, 100, 0, 0, 0, 40, 0, 0, 3, 0x20, 0, 0, 2, 0x58,
};

static void
test_ssh_chan_winch_parse(void)
{
    uint32_t d[4] = { 0, 0, 0, 0 };
    test_check(axl_ssh_chan_winch_parse(CHAN_WINCH_REST, sizeof CHAN_WINCH_REST,
                                        d) == AXL_OK
                   && d[0] == 100 && d[1] == 40 && d[2] == 800 && d[3] == 600,
               "chan winch: cols rows wpx hpx");
    test_check(chan_first_accepted_prefix(chan_winch_fn, CHAN_WINCH_REST,
                                          sizeof CHAN_WINCH_REST, 0) == -1,
               "chan winch: every truncation is refused");
    uint8_t t[sizeof CHAN_WINCH_REST + 1];
    axl_memcpy(t, CHAN_WINCH_REST, sizeof CHAN_WINCH_REST);
    t[sizeof CHAN_WINCH_REST] = 0;
    test_check(axl_ssh_chan_winch_parse(t, sizeof t, d) == AXL_ERR,
               "chan winch: a trailing byte is refused");
    test_check(axl_ssh_chan_winch_parse(CHAN_WINCH_REST,
                                        sizeof CHAN_WINCH_REST, NULL) == AXL_ERR,
               "chan winch: NULL dims is refused");
    test_check(axl_ssh_chan_winch_parse(NULL, 16, d) == AXL_ERR,
               "chan winch: NULL input is refused");
}

static int
chan_exec_fn(const uint8_t *p, size_t len)
{
    const uint8_t *c;
    uint32_t       n;
    return axl_ssh_chan_exec_parse(p, len, &c, &n);
}

static void
test_ssh_chan_exec_parse(void)
{
    static const uint8_t ok[] = { 0, 0, 0, 5, 'l','s',' ','-','l' };
    const uint8_t *c = NULL;
    uint32_t       n = 0;
    test_check(axl_ssh_chan_exec_parse(ok, sizeof ok, &c, &n) == AXL_OK
                   && c == ok + 4 && n == 5 && axl_memcmp(c, "ls -l", 5) == 0,
               "chan exec: command and length");
    test_check(chan_first_accepted_prefix(chan_exec_fn, ok, sizeof ok, 0) == -1,
               "chan exec: every truncation is refused");
    static const uint8_t empty[] = { 0, 0, 0, 0 };
    test_check(axl_ssh_chan_exec_parse(empty, sizeof empty, &c, &n) == AXL_OK
                   && n == 0, "chan exec: an empty command parses");
    uint8_t t[sizeof ok + 1];
    axl_memcpy(t, ok, sizeof ok);
    t[sizeof ok] = 0;
    test_check(axl_ssh_chan_exec_parse(t, sizeof t, &c, &n) == AXL_ERR,
               "chan exec: a trailing byte after the string is refused");

    static const uint8_t inner[] = { 0, 0, 0, 5, 'l','s',0,'-','l' };
    test_check(axl_ssh_chan_exec_parse(inner, sizeof inner, &c, &n) == AXL_ERR,
               "chan exec: a NUL inside the command is refused");
    static const uint8_t lead[] = { 0, 0, 0, 3, 0,'l','s' };
    test_check(axl_ssh_chan_exec_parse(lead, sizeof lead, &c, &n) == AXL_ERR,
               "chan exec: a leading NUL is refused");
    static const uint8_t last[] = { 0, 0, 0, 3, 'l','s',0 };
    test_check(axl_ssh_chan_exec_parse(last, sizeof last, &c, &n) == AXL_OK
                   && n == 2 && axl_memcmp(c, "ls", 2) == 0,
               "chan exec: one trailing NUL is accepted and stripped");
    static const uint8_t two[] = { 0, 0, 0, 4, 'l','s',0,0 };
    test_check(axl_ssh_chan_exec_parse(two, sizeof two, &c, &n) == AXL_ERR,
               "chan exec: two trailing NULs are refused");
    static const uint8_t only[] = { 0, 0, 0, 1, 0 };
    test_check(axl_ssh_chan_exec_parse(only, sizeof only, &c, &n) == AXL_OK
                   && n == 0, "chan exec: a lone NUL strips to empty");
    test_check(axl_ssh_chan_exec_parse(ok, sizeof ok, NULL, &n) == AXL_ERR,
               "chan exec: NULL cmd is refused");
    test_check(axl_ssh_chan_exec_parse(ok, sizeof ok, &c, NULL) == AXL_ERR,
               "chan exec: NULL cmd_len is refused");
    test_check(axl_ssh_chan_exec_parse(NULL, 9, &c, &n) == AXL_ERR,
               "chan exec: NULL input is refused");
}

static int
chan_eof_fn(const uint8_t *p, size_t len)
{
    uint32_t r;
    return axl_ssh_chan_u32_msg_parse(p, len, &r);
}

static void
test_ssh_chan_u32_msg_parse(void)
{
    static const uint8_t eof[] = { 96, 0, 0, 1, 2 };
    static const uint8_t cls[] = { 97, 0xFF, 0xFF, 0xFF, 0xFE };
    uint32_t r = 0;
    test_check(axl_ssh_chan_u32_msg_parse(eof, sizeof eof, &r) == AXL_OK
                   && r == 0x0102, "chan eof: recipient");
    test_check(axl_ssh_chan_u32_msg_parse(cls, sizeof cls, &r) == AXL_OK
                   && r == 0xFFFFFFFEu, "chan close: recipient");
    test_check(chan_first_accepted_prefix(chan_eof_fn, eof, sizeof eof, 0) == -1,
               "chan eof/close: every truncation is refused");
    uint8_t t[6] = { 96, 0, 0, 0, 1, 0 };
    test_check(axl_ssh_chan_u32_msg_parse(t, sizeof t, &r) == AXL_ERR,
               "chan eof/close: a trailing byte is refused");
    t[0] = 98;
    test_check(axl_ssh_chan_u32_msg_parse(t, 5, &r) == AXL_ERR,
               "chan eof/close: a type other than 96 or 97 is refused");
    test_check(axl_ssh_chan_u32_msg_parse(eof, sizeof eof, NULL) == AXL_ERR,
               "chan eof/close: NULL out is refused");
    test_check(axl_ssh_chan_u32_msg_parse(NULL, 5, &r) == AXL_ERR,
               "chan eof/close: NULL input is refused");
}

static int
chan_adjust_fn(const uint8_t *p, size_t len)
{
    uint32_t r, b;
    return axl_ssh_chan_adjust_parse(p, len, &r, &b);
}

static void
test_ssh_chan_adjust_parse(void)
{
    static const uint8_t m[] = { 93, 0, 0, 0, 7, 0, 1, 0, 0 };
    uint32_t r = 0, b = 0;
    test_check(axl_ssh_chan_adjust_parse(m, sizeof m, &r, &b) == AXL_OK
                   && r == 7 && b == 0x10000, "chan adjust: recipient and bytes");
    test_check(chan_first_accepted_prefix(chan_adjust_fn, m, sizeof m, 0) == -1,
               "chan adjust: every truncation is refused");
    uint8_t t[sizeof m + 1];
    axl_memcpy(t, m, sizeof m);
    t[sizeof m] = 0;
    test_check(axl_ssh_chan_adjust_parse(t, sizeof t, &r, &b) == AXL_ERR,
               "chan adjust: a trailing byte is refused");
    t[0] = 94;
    test_check(axl_ssh_chan_adjust_parse(t, sizeof m, &r, &b) == AXL_ERR,
               "chan adjust: a type other than 93 is refused");
    test_check(axl_ssh_chan_adjust_parse(m, sizeof m, NULL, &b) == AXL_ERR,
               "chan adjust: NULL recipient is refused");
    test_check(axl_ssh_chan_adjust_parse(m, sizeof m, &r, NULL) == AXL_ERR,
               "chan adjust: NULL bytes is refused");
    test_check(axl_ssh_chan_adjust_parse(NULL, 9, &r, &b) == AXL_ERR,
               "chan adjust: NULL input is refused");
}

static int
chan_data_fn(const uint8_t *p, size_t len)
{
    uint32_t r, n;
    const uint8_t *d;
    return axl_ssh_chan_data_parse(p, len, &r, &d, &n);
}

static int
chan_ext_fn(const uint8_t *p, size_t len)
{
    uint32_t r, c, n;
    const uint8_t *d;
    return axl_ssh_chan_ext_data_parse(p, len, &r, &c, &d, &n);
}

static void
test_ssh_chan_data_parse(void)
{
    static const uint8_t m[] = { 94, 0, 0, 0, 3, 0, 0, 0, 2, 'h', 'i' };
    uint32_t r = 0, n = 0;
    const uint8_t *d = NULL;
    test_check(axl_ssh_chan_data_parse(m, sizeof m, &r, &d, &n) == AXL_OK
                   && r == 3 && n == 2 && d == m + 9
                   && axl_memcmp(d, "hi", 2) == 0,
               "chan data: recipient and payload");
    test_check(chan_first_accepted_prefix(chan_data_fn, m, sizeof m, 0) == -1,
               "chan data: every truncation is refused");
    test_check(axl_ssh_chan_data_parse(m, sizeof m - 1, &r, &d, &n) == AXL_ERR,
               "chan data: a payload shorter than its length is refused");
    uint8_t t[sizeof m + 1];
    axl_memcpy(t, m, sizeof m);
    t[sizeof m] = 0;
    test_check(axl_ssh_chan_data_parse(t, sizeof t, &r, &d, &n) == AXL_ERR,
               "chan data: a trailing byte is refused");
    static const uint8_t e[] = { 94, 0, 0, 0, 3, 0, 0, 0, 0 };
    test_check(axl_ssh_chan_data_parse(e, sizeof e, &r, &d, &n) == AXL_OK
                   && n == 0, "chan data: an empty payload parses");
    t[0] = 95;
    test_check(axl_ssh_chan_data_parse(t, sizeof m, &r, &d, &n) == AXL_ERR,
               "chan data: a type other than 94 is refused");
    test_check(axl_ssh_chan_data_parse(m, sizeof m, NULL, &d, &n) == AXL_ERR,
               "chan data: NULL recipient is refused");
    test_check(axl_ssh_chan_data_parse(m, sizeof m, &r, NULL, &n) == AXL_ERR,
               "chan data: NULL data is refused");
    test_check(axl_ssh_chan_data_parse(m, sizeof m, &r, &d, NULL) == AXL_ERR,
               "chan data: NULL data_len is refused");
    test_check(axl_ssh_chan_data_parse(NULL, 11, &r, &d, &n) == AXL_ERR,
               "chan data: NULL input is refused");
}

static void
test_ssh_chan_ext_data_parse(void)
{
    static const uint8_t m[] = { 95, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 2, 'e', 'r' };
    uint32_t r = 0, c = 0, n = 0;
    const uint8_t *d = NULL;
    test_check(axl_ssh_chan_ext_data_parse(m, sizeof m, &r, &c, &d, &n)
                   == AXL_OK && r == 3 && c == 1 && n == 2 && d == m + 13
                   && axl_memcmp(d, "er", 2) == 0,
               "chan ext data: recipient, code and payload");
    test_check(chan_first_accepted_prefix(chan_ext_fn, m, sizeof m, 0) == -1,
               "chan ext data: every truncation is refused");
    test_check(axl_ssh_chan_ext_data_parse(m, 9, &r, &c, &d, &n) == AXL_ERR,
               "chan ext data: a missing payload string is refused");
    uint8_t t[sizeof m + 1];
    axl_memcpy(t, m, sizeof m);
    t[sizeof m] = 0;
    test_check(axl_ssh_chan_ext_data_parse(t, sizeof t, &r, &c, &d, &n)
                   == AXL_ERR, "chan ext data: a trailing byte is refused");
    t[0] = 94;
    test_check(axl_ssh_chan_ext_data_parse(t, sizeof m, &r, &c, &d, &n)
                   == AXL_ERR, "chan ext data: a type other than 95 is refused");
    test_check(axl_ssh_chan_ext_data_parse(m, sizeof m, &r, NULL, &d, &n)
                   == AXL_ERR, "chan ext data: NULL code is refused");
    test_check(axl_ssh_chan_ext_data_parse(NULL, 15, &r, &c, &d, &n) == AXL_ERR,
               "chan ext data: NULL input is refused");
}

static void
test_ssh_chan_builders(void)
{
    AxlString *b = axl_string_new("");

    static const uint8_t confirm[] = {
        91, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0x80, 0, 0, 0, 0x40, 0 };
    test_check(axl_ssh_chan_open_confirm_build(b, 1, 2, 0x8000, 0x4000) == AXL_OK
                   && chan_bytes_eq(b, confirm, sizeof confirm),
               "chan build: OPEN_CONFIRMATION exact bytes");

    axl_string_truncate(b, 0);
    static const uint8_t fail[] = {
        92, 0, 0, 0, 5,                     /* recipient */
        0, 0, 0, 4,                         /* reason: resource shortage */
        0, 0, 0, 0,                         /* description "" */
        0, 0, 0, 0 };                       /* language "" */
    test_check(axl_ssh_chan_open_failure_build(b, 5,
                                               AXL_SSH_OPEN_RESOURCE_SHORTAGE)
                   == AXL_OK && chan_bytes_eq(b, fail, sizeof fail),
               "chan build: OPEN_FAILURE exact bytes");

    axl_string_truncate(b, 0);
    static const uint8_t data[] = { 94, 0, 0, 0, 3, 0, 0, 0, 2, 'o', 'k' };
    test_check(axl_ssh_chan_data_build(b, 3, false, "ok", 2) == AXL_OK
                   && chan_bytes_eq(b, data, sizeof data),
               "chan build: DATA exact bytes");

    axl_string_truncate(b, 0);
    static const uint8_t ext[] = {
        95, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 3, 'e', 'r', 'r' };
    test_check(axl_ssh_chan_data_build(b, 3, true, "err", 3) == AXL_OK
                   && chan_bytes_eq(b, ext, sizeof ext),
               "chan build: stderr EXTENDED_DATA exact bytes");

    axl_string_truncate(b, 0);
    static const uint8_t empty[] = { 94, 0, 0, 0, 3, 0, 0, 0, 0 };
    test_check(axl_ssh_chan_data_build(b, 3, false, NULL, 0) == AXL_OK
                   && chan_bytes_eq(b, empty, sizeof empty),
               "chan build: empty DATA with a NULL buffer");

    axl_string_truncate(b, 0);
    static const uint8_t adj[] = { 93, 0, 0, 0, 7, 0, 1, 0, 0 };
    test_check(axl_ssh_chan_adjust_build(b, 7, 0x10000) == AXL_OK
                   && chan_bytes_eq(b, adj, sizeof adj),
               "chan build: WINDOW_ADJUST exact bytes");

    static const uint8_t types[4] = { 96, 97, 99, 100 };
    for (size_t i = 0; i < 4; i++) {
        axl_string_truncate(b, 0);
        const uint8_t want[] = { types[i], 0xAA, 0xBB, 0xCC, 0xDD };
        test_check(axl_ssh_chan_u32_msg_build(b, types[i], 0xAABBCCDDu) == AXL_OK
                       && chan_bytes_eq(b, want, sizeof want),
                   "chan build: EOF/CLOSE/SUCCESS/FAILURE exact bytes");
    }
    axl_string_truncate(b, 0);
    test_check(axl_ssh_chan_u32_msg_build(b, 94, 1) == AXL_ERR
                   && axl_string_len(b) == 0,
               "chan build: a type that is not a bare-recipient one is refused");

    axl_string_truncate(b, 0);
    static const uint8_t es[] = {
        98, 0, 0, 0, 4,                     /* CHANNEL_REQUEST, recipient */
        0, 0, 0, 11, 'e','x','i','t','-','s','t','a','t','u','s',
        0,                                  /* want_reply FALSE */
        0, 0, 0, 42 };                      /* exit status */
    test_check(axl_ssh_chan_exit_status_build(b, 4, 42) == AXL_OK
                   && chan_bytes_eq(b, es, sizeof es),
               "chan build: exit-status exact bytes");

    /* sshd's client_alive_check, byte for byte: a CHANNEL_REQUEST on the
       probe target, or a GLOBAL_REQUEST; want_reply TRUE, no data. */
    axl_string_truncate(b, 0);
    static const uint8_t ka_chan[] = {
        98, 0, 0, 0, 7,                     /* CHANNEL_REQUEST, recipient */
        0, 0, 0, 21, 'k','e','e','p','a','l','i','v','e','@',
        'o','p','e','n','s','s','h','.','c','o','m',
        1 };                                /* want_reply TRUE */
    test_check(axl_ssh_keepalive_build(b, true, 7) == AXL_OK
                   && chan_bytes_eq(b, ka_chan, sizeof ka_chan),
               "chan build: keepalive CHANNEL_REQUEST exact bytes");
    axl_string_truncate(b, 0);
    static const uint8_t ka_glob[] = {
        80,                                 /* GLOBAL_REQUEST */
        0, 0, 0, 21, 'k','e','e','p','a','l','i','v','e','@',
        'o','p','e','n','s','s','h','.','c','o','m',
        1 };                                /* want_reply TRUE */
    test_check(axl_ssh_keepalive_build(b, false, 7) == AXL_OK
                   && chan_bytes_eq(b, ka_glob, sizeof ka_glob),
               "chan build: keepalive GLOBAL_REQUEST exact bytes");
    axl_string_truncate(b, 0);

    test_check(axl_ssh_chan_data_build(NULL, 3, false, "x", 1) == AXL_ERR,
               "chan build: NULL builder is refused");
    test_check(axl_ssh_chan_data_build(b, 3, false, NULL, 1) == AXL_ERR,
               "chan build: NULL data with a length is refused");
    axl_string_free(b);
}

/* ------------------------------------------------------------------------ */
/* SSH channel table (axl-ssh-chan.h)                                        */
/* ------------------------------------------------------------------------ */

#define CHAN_TAB 4u

/* Drain @a n bytes from @a ch exactly as the server will: plan a packet,
   send it, repeat. The peer window and max packet are opened wide first so
   only the queue decides. */
static void
chan_drain(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b,
    size_t            n
)
{
    ch->peer_window = UINT32_MAX;
    ch->peer_maxpkt = UINT32_MAX;
    while (n > 0) {
        bool   err  = false;
        size_t take = axl_ssh_chan_plan_send(ch, &err);
        if (take == 0) {
            return;
        }
        if (take > n) {
            take = n;
        }
        axl_ssh_chan_sent(ch, b, err, take);
        n -= take;
    }
}

/* Drain @a ch down to @a left queued bytes. */
static void
chan_drain_to(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b,
    size_t            left
)
{
    if (ch->queued > left) {
        chan_drain(ch, b, ch->queued - left);
    }
}

static void
chan_free_all(
    AxlSshChanSlot   *tab,
    AxlSshChanBudget *b
)
{
    for (unsigned i = 0; i < CHAN_TAB; i++) {
        axl_ssh_chan_free(&tab[i], b);
    }
}

static void
test_ssh_chan_ids(void)
{
    axl_ssh_chan_set_next_id(UINT32_MAX);
    uint32_t a = axl_ssh_chan_next_id();
    uint32_t b = axl_ssh_chan_next_id();
    uint32_t c = axl_ssh_chan_next_id();
    test_check(a == UINT32_MAX, "chan id: the seeded id UINT32_MAX is issued");
    test_check(b == 1, "chan id: the wrap past UINT32_MAX skips 0 and issues 1");
    test_check(c == 2, "chan id: then 2 -- ids are distinct and never 0");
    axl_ssh_chan_set_next_id(0);
    test_check(axl_ssh_chan_next_id() == 1, "chan id: a seed of 0 issues 1, never 0");
    axl_ssh_chan_set_next_id(1000);
}

static void
test_ssh_chan_budget_init(void)
{
    AxlSshChanBudget b;
    axl_ssh_chan_budget_init(&b, 256u * 1024u, 4u * 1024u * 1024u, 160);
    test_check(b.limit_per_channel == 262144 && b.limit_total == 4194304
                   && b.queued_total == 0,
               "chan budget: init sets both limits, nothing queued");
    test_check(b.fair_share == 26214,
               "chan budget: the floor is 4 MB / 160 = 26214 bytes at the defaults");
    axl_ssh_chan_budget_init(&b, 100, 159, 160);
    test_check(b.fair_share == 0, "chan budget: a total below 160 makes the floor 0");
    axl_ssh_chan_budget_init(&b, 100, 1000, 0);
    test_check(b.fair_share == 0, "chan budget: no channel count means no floor");
}

static void
test_ssh_chan_alloc(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget bud;
    axl_memset(tab, 0, sizeof tab);
    axl_ssh_chan_budget_init(&bud, 1000, 1000, 0);

    AxlSshChanSlot *a = axl_ssh_chan_alloc(tab, CHAN_TAB, 3);
    AxlSshChanSlot *b = axl_ssh_chan_alloc(tab, CHAN_TAB, 3);
    AxlSshChanSlot *c = axl_ssh_chan_alloc(tab, CHAN_TAB, 3);
    test_check(a != NULL && b != NULL && c != NULL,
               "chan alloc: max_sessions (3) slots are allocated");
    if (a == NULL || b == NULL || c == NULL) {
        chan_free_all(tab, &bud);
        return;
    }
    test_check(a == &tab[0] && b == &tab[1] && c == &tab[2]
                   && a->local_id == 0 && b->local_id == 1 && c->local_id == 2,
               "chan alloc: slots fill in order; the wire number is the slot index");
    test_check(a->id != 0 && b->id != 0 && c->id != 0 && a->id != b->id
                   && b->id != c->id && a->id != c->id,
               "chan alloc: handles are distinct and never 0");
    test_check(a->local_window == 0 && !a->started && a->queued == 0,
               "chan alloc: a new channel has window 0 and is not started");
    test_check(axl_ssh_chan_alloc(tab, CHAN_TAB, 3) == NULL,
               "chan alloc: the slot past max_sessions is refused");

    test_check(axl_ssh_chan_find(tab, CHAN_TAB, b->id) == b,
               "chan find: a live handle finds its slot");
    test_check(axl_ssh_chan_find(tab, CHAN_TAB, 0) == NULL,
               "chan find: handle 0 finds nothing");
    test_check(axl_ssh_chan_find_peer(tab, CHAN_TAB, 2) == c,
               "chan find_peer: a wire number finds its slot");
    test_check(axl_ssh_chan_find_peer(tab, CHAN_TAB, 3) == NULL
                   && axl_ssh_chan_find_peer(tab, CHAN_TAB, CHAN_TAB) == NULL,
               "chan find_peer: a free slot or an out-of-range number finds nothing");

    uint32_t old_b = b->id;
    axl_ssh_chan_free(b, &bud);
    test_check(axl_ssh_chan_find(tab, CHAN_TAB, old_b) == NULL,
               "chan find: a freed handle finds nothing (stale handle)");
    test_check(axl_ssh_chan_find_peer(tab, CHAN_TAB, 1) == NULL,
               "chan find_peer: a freed slot's wire number finds nothing");
    AxlSshChanSlot *b2 = axl_ssh_chan_alloc(tab, CHAN_TAB, 3);
    test_check(b2 == &tab[1] && b2->local_id == 1 && b2->id != old_b
                   && b2->id != 0,
               "chan alloc: a freed slot is reused, under a NEW handle");
    test_check(axl_ssh_chan_find(tab, CHAN_TAB, old_b) == NULL,
               "chan find: the old handle still finds nothing after reuse");

    uint32_t old_c = c->id;
    axl_ssh_chan_retire(c, &bud);
    test_check(axl_ssh_chan_find(tab, CHAN_TAB, old_c) == NULL,
               "chan retired: find by handle returns NULL");
    test_check(axl_ssh_chan_find_peer(tab, CHAN_TAB, 2) == c,
               "chan retired: find_peer by wire number still returns the slot");
    test_check(axl_ssh_chan_alloc(tab, CHAN_TAB, 3) == NULL,
               "chan retired: the slot still counts against max_sessions");
    axl_ssh_chan_free(c, &bud);
    test_check(axl_ssh_chan_alloc(tab, CHAN_TAB, 3) == &tab[2],
               "chan alloc: freeing the retired slot (both CLOSEs) makes room");

    chan_free_all(tab, &bud);
    for (unsigned i = 0; i < CHAN_TAB; i++) {
        test_check(axl_ssh_chan_alloc(tab, CHAN_TAB, 99) != NULL,
                   "chan alloc: a session cap above the table is bounded by it (fill)");
    }
    test_check(axl_ssh_chan_alloc(tab, CHAN_TAB, 99) == NULL,
               "chan alloc: a full table refuses whatever the session cap");
    test_check(axl_ssh_chan_alloc(NULL, CHAN_TAB, 3) == NULL,
               "chan alloc: a NULL table is refused");
    chan_free_all(tab, &bud);
}

static void
test_ssh_chan_write(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    axl_memset(tab, 0, sizeof tab);
    axl_ssh_chan_budget_init(&b, 256, 100000, 0);
    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    uint8_t         buf[700];
    axl_memset(buf, 'x', sizeof buf);
    if (ch == NULL) {
        test_check(false, "chan write: alloc");
        return;
    }

    test_check(axl_ssh_chan_write(ch, &b, false, NULL, 0) == AXL_OK
                   && ch->queued == 0 && b.queued_total == 0,
               "chan write: len 0 is AXL_OK and queues nothing");
    test_check(axl_ssh_chan_write(ch, &b, false, NULL, 1) == AXL_INVALID,
               "chan write: a NULL buf with len 1 is AXL_INVALID");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 257) == AXL_ERR
                   && ch->queued == 0 && !ch->busy_on_chan && !ch->busy_on_total,
               "chan write: len above the channel limit is AXL_ERR and owes nothing");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 255) == AXL_OK
                   && ch->queued == 255 && b.queued_total == 255,
               "chan write: limit-1 bytes queue");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 2) == AXL_BUSY
                   && ch->queued == 255 && b.queued_total == 255,
               "chan write: 2 more is AXL_BUSY, all or nothing at the channel limit");
    test_check(ch->busy_on_chan && !ch->busy_on_total && ch->busy_len == 2,
               "chan write: the refusal is recorded on the channel limit");
    test_check(axl_ssh_chan_write(ch, &b, true, buf, 1) == AXL_OK
                   && ch->queued == 256 && b.queued_total == 256,
               "chan write: stderr fills the same limit to exactly 256");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 1) == AXL_BUSY
                   && axl_ssh_chan_write(ch, &b, true, buf, 1) == AXL_BUSY
                   && ch->queued == 256,
               "chan write: stdout and stderr both refused at the shared limit");
    chan_free_all(tab, &b);
    test_check(b.queued_total == 0, "chan write: free returns every queued byte");

    /* The server total, across two channels. */
    axl_ssh_chan_budget_init(&b, 600, 1000, 0);
    AxlSshChanSlot *c1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *c2 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(c1, &b, false, buf, 600) == AXL_OK
                   && axl_ssh_chan_write(c2, &b, false, buf, 400) == AXL_OK
                   && b.queued_total == 1000,
               "chan write: two channels fill the 1000-byte total");
    test_check(axl_ssh_chan_write(c2, &b, false, buf, 1) == AXL_BUSY
                   && c2->queued == 400 && b.queued_total == 1000,
               "chan write: one byte past the total is AXL_BUSY, nothing queued");
    test_check(c2->busy_on_total && !c2->busy_on_chan && c2->busy_had_queue,
               "chan write: the refusal is on the total, with the queue non-empty");
    chan_free_all(tab, &b);

    /* The fair-share floor: the total exhausted by others, a channel
       holding less than the floor may still queue up to it. */
    axl_ssh_chan_budget_init(&b, 600, 1000, 0);
    b.fair_share = 100;
    c1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    c2 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *c3 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(c1, &b, false, buf, 600) == AXL_OK
                   && axl_ssh_chan_write(c2, &b, false, buf, 400) == AXL_OK,
               "chan floor: the total is exhausted by other channels");
    test_check(axl_ssh_chan_write(c3, &b, false, buf, 40) == AXL_OK
                   && axl_ssh_chan_write(c3, &b, true, buf, 60) == AXL_OK
                   && c3->queued == 100 && b.queued_total == 1100,
               "chan floor: a channel under the floor queues up to it (100) past the total");
    test_check(axl_ssh_chan_write(c3, &b, false, buf, 1) == AXL_BUSY
                   && c3->queued == 100 && b.queued_total == 1100,
               "chan floor: one byte beyond the floor is AXL_BUSY");
    test_check(c3->busy_on_total && !c3->busy_on_chan,
               "chan floor: that refusal is on the total");
    chan_free_all(tab, &b);

    /* Ended output, a free slot, a retired one, NULL arguments. */
    axl_ssh_chan_budget_init(&b, 600, 1000, 0);
    c1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    c1->exit_pending = true;
    test_check(axl_ssh_chan_write(c1, &b, false, buf, 1) == AXL_ERR
                   && c1->queued == 0,
               "chan write: after exit is AXL_ERR");
    c1->exit_pending = false;
    axl_ssh_chan_retire(c1, &b);
    test_check(axl_ssh_chan_write(c1, &b, false, buf, 1) == AXL_NOT_FOUND,
               "chan write: a retired channel is AXL_NOT_FOUND");
    test_check(axl_ssh_chan_write(&tab[3], &b, false, buf, 1) == AXL_NOT_FOUND,
               "chan write: a free slot is AXL_NOT_FOUND");
    test_check(axl_ssh_chan_write(NULL, &b, false, buf, 1) == AXL_INVALID
                   && axl_ssh_chan_write(&tab[3], NULL, false, buf, 1) == AXL_INVALID,
               "chan write: NULL channel or budget is AXL_INVALID");
    chan_free_all(tab, &b);

    /* Memory: every result is all or nothing. Fail the 1st, 2nd, ...
       allocation of a write onto an existing queue and of a first write. */
    unsigned refused = 0;
    bool     reached = false;
    for (size_t n = 1; n <= 8 && !reached; n++) {
        axl_ssh_chan_budget_init(&b, 600, 1000, 0);
        c1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
        axl_mem_fail_next_alloc(n);
        int r1 = axl_ssh_chan_write(c1, &b, false, buf, 3);
        int r2 = axl_ssh_chan_write(c1, &b, true, buf, 500);
        axl_mem_fail_next_alloc(0);
        size_t want = (r1 == AXL_OK ? 3u : 0u) + (r2 == AXL_OK ? 500u : 0u);
        size_t runs = (r1 == AXL_OK ? 5u + 3u : 0u) + (r2 == AXL_OK ? 5u + 500u : 0u);
        size_t have = c1->out != NULL ? axl_string_len(c1->out) : 0;
        if ((r1 != AXL_OK && r1 != AXL_NO_RESOURCES)
            || (r2 != AXL_OK && r2 != AXL_NO_RESOURCES)
            || c1->queued != want || b.queued_total != want || have != runs) {
            test_check(false, "chan write: an allocation failure leaves nothing behind");
            refused = 0;
            chan_free_all(tab, &b);
            break;
        }
        if (r1 == AXL_OK && r2 == AXL_OK) {
            reached = true;
        } else {
            refused++;
        }
        chan_free_all(tab, &b);
    }
    test_check(reached && refused >= 1,
               "chan write: an allocation failure is AXL_NO_RESOURCES, all or nothing");
}

static void
test_ssh_chan_writable(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    uint8_t          buf[1000];
    axl_memset(tab, 0, sizeof tab);
    axl_memset(buf, 'w', sizeof buf);

    /* The channel limit: below half, then exactly once. */
    axl_ssh_chan_budget_init(&b, 256, 100000, 0);
    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan writable: false with no refusal");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 256) == AXL_OK
                   && axl_ssh_chan_write(ch, &b, false, buf, 1) == AXL_BUSY,
               "chan writable: a refusal on the channel limit");
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan writable: false right after the refusal (queue 256)");
    chan_drain_to(ch, &b, 128);
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan writable: false at exactly half the limit (128)");
    chan_drain_to(ch, &b, 127);
    test_check(axl_ssh_chan_take_writable(ch, &b),
               "chan writable: true once the queue is under half (127)");
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan writable: true exactly once -- the edge is consumed");
    chan_free_all(tab, &b);

    /* The FIT clause on the channel limit: limit 256, queue 100, a refused
       write of 200. 100 < 128 already, but 300 does not fit. */
    axl_ssh_chan_budget_init(&b, 256, 100000, 0);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 100) == AXL_OK
                   && axl_ssh_chan_write(ch, &b, false, buf, 200) == AXL_BUSY,
               "chan fit: 200 onto 100 under a 256 limit is refused");
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan fit (channel): false at queue 100 although below half -- 300 > 256");
    chan_drain_to(ch, &b, 57);
    test_check(!axl_ssh_chan_take_writable(ch, &b),
               "chan fit (channel): false at queue 57 -- 257 > 256");
    chan_drain_to(ch, &b, 56);
    test_check(axl_ssh_chan_take_writable(ch, &b),
               "chan fit (channel): true at queue 56 -- the refused 200 now fits");
    test_check(axl_ssh_chan_write(ch, &b, false, buf, 200) == AXL_OK,
               "chan fit (channel): the promise -- the retry succeeds");
    chan_free_all(tab, &b);

    /* The FIT clause on the total: total = per-channel = 1000, 400 queued
       elsewhere, a refused write of 700 on an EMPTY channel. */
    axl_ssh_chan_budget_init(&b, 1000, 1000, 0);
    AxlSshChanSlot *a  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *ob = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ob, &b, false, buf, 400) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 700) == AXL_BUSY
                   && a->busy_on_total && !a->busy_had_queue,
               "chan fit: 700 onto a total holding 400 is refused on the total");
    chan_drain_to(ob, &b, 350);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan fit (total): false at total 350 although below half -- 1050 > 1000");
    chan_drain_to(ob, &b, 300);
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan fit (total): true at total 300 -- the refused 700 now fits");
    chan_free_all(tab, &b);

    /* Total refusal with the queue NON-empty: draining to empty releases
       it, whatever the total (the fair-share floor becomes reachable). */
    axl_ssh_chan_budget_init(&b, 600, 1000, 0);
    a  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    ob = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ob, &b, false, buf, 600) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 300) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 200) == AXL_BUSY
                   && a->busy_on_total && a->busy_had_queue,
               "chan drain: refused on the total while the queue held 300");
    chan_drain_to(a, &b, 1);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan drain: false with one byte still queued (total 601)");
    chan_drain_to(a, &b, 0);
    test_check(b.queued_total == 600 && axl_ssh_chan_take_writable(a, &b),
               "chan drain: true once the queue drains to empty, total still 600");
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan drain: and only once");

    /* Any refusal on the total since the last writable: the queue held
       bytes at the FIRST, was empty at the second; the drain still counts. */
    test_check(axl_ssh_chan_write(a, &b, false, buf, 100) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 400) == AXL_BUSY,
               "chan drain: a refusal on the total with 100 queued");
    chan_drain_to(a, &b, 0);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 450) == AXL_BUSY,
               "chan drain: a second refusal, made with the queue empty");
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan drain: the earlier non-empty refusal still releases on the drain");
    chan_free_all(tab, &b);

    /* Total refusal with the queue EMPTY: no drain release, and repeated
       calls with nothing changed stay false (no spin). */
    axl_ssh_chan_budget_init(&b, 600, 1000, 0);
    a  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    ob = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *oc = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ob, &b, false, buf, 600) == AXL_OK
                   && axl_ssh_chan_write(oc, &b, false, buf, 300) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 200) == AXL_BUSY
                   && a->busy_on_total && !a->busy_had_queue,
               "chan no-spin: refused on the total with the queue empty");
    bool spun = false;
    for (int i = 0; i < 3; i++) {
        spun = spun || axl_ssh_chan_take_writable(a, &b);
    }
    test_check(!spun, "chan no-spin: an empty queue alone never fires, call after call");
    chan_drain_to(oc, &b, 0);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan no-spin: false at total 600 -- not below half");
    chan_drain_to(ob, &b, 499);
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan no-spin: true once the total is under half (499) and 200 fits");
    chan_free_all(tab, &b);

    /* Refusals on BOTH limits since the last writable: both conditions.
       Three other channels hold 768 (256 each, the channel limit), A holds
       100; A is refused 200 on its own limit, then 150 on the total
       (868 + 150 > 1000). L = 200: the channel needs A <= 56, the total
       needs T < 500 and T + 200 <= 1000. */
    for (int order = 0; order < 2; order++) {
        axl_ssh_chan_budget_init(&b, 256, 1000, 0);
        a                   = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
        AxlSshChanSlot *o1  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
        AxlSshChanSlot *o2  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
        AxlSshChanSlot *o3  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
        bool setup = axl_ssh_chan_write(o1, &b, false, buf, 256) == AXL_OK
                     && axl_ssh_chan_write(o2, &b, false, buf, 256) == AXL_OK
                     && axl_ssh_chan_write(o3, &b, false, buf, 256) == AXL_OK
                     && axl_ssh_chan_write(a, &b, false, buf, 100) == AXL_OK
                     && axl_ssh_chan_write(a, &b, false, buf, 200) == AXL_BUSY
                     && axl_ssh_chan_write(a, &b, false, buf, 150) == AXL_BUSY
                     && a->busy_on_chan && a->busy_on_total && a->busy_len == 200;
        test_check(setup, "chan both: refused on the channel (200) then the total (150)");
        if (order == 0) {
            chan_drain_to(a, &b, 56);
            test_check(!axl_ssh_chan_take_writable(a, &b),
                       "chan both: the channel condition alone is not enough (total 824)");
            chan_drain_to(o1, &b, 0);
            chan_drain_to(o2, &b, 0);
        } else {
            chan_drain_to(o1, &b, 0);
            chan_drain_to(o2, &b, 0);
            test_check(!axl_ssh_chan_take_writable(a, &b),
                       "chan both: the total condition alone is not enough (queue 100)");
            chan_drain_to(a, &b, 56);
        }
        test_check(b.queued_total == 312 && axl_ssh_chan_take_writable(a, &b),
                   "chan both: true once both hold (queue 56, total 312)");
        chan_free_all(tab, &b);
    }

    /* "Below half" without integer halving: every limit is reachable. */
    axl_ssh_chan_budget_init(&b, 1, 1000, 0);
    a = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_BUSY,
               "chan reach: a channel limit of 1 refuses the second byte");
    chan_drain_to(a, &b, 0);
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan reach: limit 1 -- writable fires once the queue drains to 0");
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan reach: limit 1 -- and only once");
    chan_free_all(tab, &b);

    axl_ssh_chan_budget_init(&b, 257, 100000, 0);
    a = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 257) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_BUSY,
               "chan reach: an odd limit (257) refuses at 257");
    chan_drain_to(a, &b, 129);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan reach: limit 257 -- false at 129 (258 > 257)");
    chan_drain_to(a, &b, 128);
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan reach: limit 257 -- true at 128 (256 < 257), not half a byte late");
    chan_free_all(tab, &b);

    axl_ssh_chan_budget_init(&b, 1, 1, 0);
    a  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    ob = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ob, &b, false, buf, 1) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_BUSY
                   && a->busy_on_total && !a->busy_had_queue,
               "chan reach: a total of 1 refuses a second channel's byte");
    chan_drain_to(ob, &b, 0);
    test_check(axl_ssh_chan_take_writable(a, &b),
               "chan reach: total 1 -- writable fires once the total drains to 0");
    chan_free_all(tab, &b);

    /* A refusal on the channel limit that the total would ALSO have
       refused owes both conditions: firing on the channel drain alone
       hands the application a retry the total still refuses. Limit 1000,
       total 1200, floor 300: A queues 900, three others 300 each under the
       floor (T = 1800); A's write of 200 fails both. */
    axl_ssh_chan_budget_init(&b, 1000, 1200, 0);
    b.fair_share = 300;
    a                  = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *p1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *p2 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *p3 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 900) == AXL_OK
                   && axl_ssh_chan_write(p1, &b, false, buf, 300) == AXL_OK
                   && axl_ssh_chan_write(p2, &b, false, buf, 300) == AXL_OK
                   && axl_ssh_chan_write(p3, &b, false, buf, 300) == AXL_OK
                   && b.queued_total == 1800
                   && axl_ssh_chan_write(a, &b, false, buf, 200) == AXL_BUSY,
               "chan both-at-once: 200 onto 900 is over the limit (1000) AND the total");
    test_check(a->busy_on_chan && a->busy_on_total && a->busy_had_queue,
               "chan both-at-once: the channel refusal also records the total");
    chan_drain_to(a, &b, 499);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan both-at-once: no writable on the channel drain alone (A 499, T 1399)");
    chan_drain_to(p1, &b, 0);
    chan_drain_to(p2, &b, 0);
    chan_drain_to(p3, &b, 0);
    test_check(b.queued_total == 499 && axl_ssh_chan_take_writable(a, &b),
               "chan both-at-once: fires once both fit (A 499, T 499)");
    test_check(axl_ssh_chan_write(a, &b, false, buf, 200) == AXL_OK,
               "chan both-at-once: the promise -- the retry succeeds");
    chan_free_all(tab, &b);

    /* Nothing is owed once the channel has exited or retired. */
    axl_ssh_chan_budget_init(&b, 256, 1000, 0);
    a = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 256) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_BUSY,
               "chan writable: refused before exit");
    a->exit_pending = true;
    chan_drain_to(a, &b, 0);
    test_check(!axl_ssh_chan_take_writable(a, &b) && !a->busy_on_chan,
               "chan writable: nothing is owed after exit");
    chan_free_all(tab, &b);
    a = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(a, &b, false, buf, 256) == AXL_OK
                   && axl_ssh_chan_write(a, &b, false, buf, 1) == AXL_BUSY,
               "chan writable: refused before retire");
    axl_ssh_chan_retire(a, &b);
    test_check(!axl_ssh_chan_take_writable(a, &b),
               "chan writable: nothing is owed after retire");
    chan_free_all(tab, &b);
}

static void
test_ssh_chan_plan_send(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    uint8_t          big[40000];
    bool             err = true;
    axl_memset(tab, 0, sizeof tab);
    axl_memset(big, 'p', sizeof big);
    axl_ssh_chan_budget_init(&b, 100000, 100000, 0);

    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_plan_send(ch, &err) == 0 && axl_ssh_chan_head(ch) == NULL,
               "chan plan: nothing queued plans 0");
    ch->peer_maxpkt = 100000;
    ch->peer_window = 0;
    test_check(axl_ssh_chan_write(ch, &b, false, big, 10) == AXL_OK
                   && axl_ssh_chan_plan_send(ch, &err) == 0,
               "chan plan: 0 when the peer window is 0");
    chan_free_all(tab, &b);

    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    ch->peer_window = 100000;
    ch->peer_maxpkt = 100000;
    err             = true;
    test_check(axl_ssh_chan_write(ch, &b, false, big, 40000) == AXL_OK
                   && axl_ssh_chan_plan_send(ch, &err) == 32768 && !err,
               "chan plan: at most 32 KB per packet");
    ch->peer_maxpkt = 1000;
    test_check(axl_ssh_chan_plan_send(ch, &err) == 1000,
               "chan plan: at most the peer's max packet (1000)");
    ch->peer_window = 500;
    test_check(axl_ssh_chan_plan_send(ch, &err) == 500,
               "chan plan: at most the peer's window (500)");
    ch->peer_maxpkt = 0;
    test_check(axl_ssh_chan_plan_send(ch, &err) == 0,
               "chan plan: 0 when the peer's max packet is 0");
    chan_free_all(tab, &b);

    /* Write order across both streams; a packet never spans two runs. */
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    ch->peer_window = 100000;
    ch->peer_maxpkt = 100000;
    test_check(axl_ssh_chan_write(ch, &b, false, "AAA", 3) == AXL_OK
                   && axl_ssh_chan_write(ch, &b, true, "BB", 2) == AXL_OK
                   && axl_ssh_chan_write(ch, &b, false, "C", 1) == AXL_OK
                   && ch->queued == 6 && b.queued_total == 6,
               "chan order: stdout, stderr, stdout queued");
    err = true;
    size_t n = axl_ssh_chan_plan_send(ch, &err);
    test_check(n == 3 && !err && axl_memcmp(axl_ssh_chan_head(ch), "AAA", 3) == 0,
               "chan order: packet 1 is stdout \"AAA\"");
    axl_ssh_chan_sent(ch, &b, err, n);
    n = axl_ssh_chan_plan_send(ch, &err);
    test_check(n == 2 && err && axl_memcmp(axl_ssh_chan_head(ch), "BB", 2) == 0,
               "chan order: packet 2 is stderr \"BB\"");
    axl_ssh_chan_sent(ch, &b, err, n);
    n = axl_ssh_chan_plan_send(ch, &err);
    test_check(n == 1 && !err && axl_memcmp(axl_ssh_chan_head(ch), "C", 1) == 0,
               "chan order: packet 3 is stdout \"C\"");
    axl_ssh_chan_sent(ch, &b, err, n);
    test_check(axl_ssh_chan_plan_send(ch, &err) == 0 && ch->queued == 0
                   && b.queued_total == 0 && ch->out == NULL,
               "chan order: then nothing; queue and total empty, its memory released");

    test_check(axl_ssh_chan_write(ch, &b, false, "ab", 2) == AXL_OK
                   && axl_ssh_chan_write(ch, &b, false, "cd", 2) == AXL_OK
                   && axl_ssh_chan_plan_send(ch, &err) == 4 && !err
                   && axl_memcmp(axl_ssh_chan_head(ch), "abcd", 4) == 0,
               "chan order: adjacent writes to one stream leave as one packet");
    axl_ssh_chan_sent(ch, &b, false, 4);

    /* A run split by the window resumes where it stopped. */
    ch->peer_window = 3;
    test_check(axl_ssh_chan_write(ch, &b, true, "12345", 5) == AXL_OK
                   && axl_ssh_chan_plan_send(ch, &err) == 3 && err,
               "chan split: the window cuts the run at 3");
    axl_ssh_chan_sent(ch, &b, true, 3);
    test_check(ch->peer_window == 0 && axl_ssh_chan_plan_send(ch, &err) == 0,
               "chan split: window spent, nothing more");
    ch->peer_window = 10;
    n = axl_ssh_chan_plan_send(ch, &err);
    test_check(n == 2 && err && axl_memcmp(axl_ssh_chan_head(ch), "45", 2) == 0,
               "chan split: the rest of the run, \"45\", after an adjust");

    /* sent debits exactly; a mismatched call changes nothing. */
    axl_ssh_chan_sent(ch, &b, false, 2);
    test_check(ch->queued == 2 && ch->peer_window == 10 && b.queued_total == 2,
               "chan sent: the wrong stream changes nothing");
    axl_ssh_chan_sent(ch, &b, true, 3);
    test_check(ch->queued == 2 && ch->peer_window == 10 && b.queued_total == 2,
               "chan sent: more than the head run changes nothing");
    axl_ssh_chan_sent(ch, &b, true, 2);
    test_check(ch->queued == 0 && ch->peer_window == 8 && b.queued_total == 0,
               "chan sent: debits window (10 -> 8), queue and total (2 -> 0) exactly");
    chan_free_all(tab, &b);

    /* A drained queue does not keep its high-water buffer. */
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(ch, &b, false, big, 20000) == AXL_OK
                   && axl_string_capacity(ch->out) >= 20000,
               "chan trim: a 20000-byte burst grows the buffer");
    chan_drain_to(ch, &b, 100);
    test_check(ch->queued == 100 && axl_string_capacity(ch->out) < 4096,
               "chan trim: draining to 100 shrinks it below 4096");
    chan_drain_to(ch, &b, 0);
    test_check(ch->out == NULL && b.queued_total == 0,
               "chan trim: draining to empty frees it");
    test_check(axl_ssh_chan_write(ch, &b, true, "xyz", 3) == AXL_OK
                   && axl_ssh_chan_plan_send(ch, &err) == 3 && err
                   && axl_memcmp(axl_ssh_chan_head(ch), "xyz", 3) == 0,
               "chan trim: a write after the free queues again");
    chan_free_all(tab, &b);
}

static void
test_ssh_chan_inbound(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    uint32_t         adj = 99;
    axl_memset(tab, 0, sizeof tab);
    axl_ssh_chan_budget_init(&b, 1000, 1000, 0);

    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_inbound(ch, 100, false, true, &adj) == AXL_SSH_CHAN_IN_DISCARD
                   && ch->local_window == 0 && ch->credit == 0 && adj == 0,
               "chan in: before start, data is ignored and NOT counted");
    test_check(axl_ssh_chan_inbound(ch, 100, true, true, &adj) == AXL_SSH_CHAN_IN_DISCARD
                   && ch->credit == 0,
               "chan in: before start, extended data too");

    axl_ssh_chan_start(ch, &adj);
    test_check(ch->started && adj == 2097152 && ch->local_window == 2097152,
               "chan in: start grants 2 MB and asks for one adjust of 2 MB");
    axl_ssh_chan_start(ch, &adj);
    test_check(adj == 0 && ch->local_window == 2097152,
               "chan in: a second start asks for nothing");

    test_check(axl_ssh_chan_inbound(ch, 32769, false, true, &adj) == AXL_SSH_CHAN_IN_DROP
                   && ch->local_window == 2097152 && ch->credit == 0,
               "chan in: a packet over 32 KB is dropped and not counted");
    test_check(axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK
                   && adj == 0 && ch->local_window == 2097152 - 32768
                   && ch->credit == 32768,
               "chan in: a 32 KB packet within the window is delivered and counted");
    bool ok = true;
    for (int i = 1; i < 3; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK && adj == 0;
    }
    test_check(ok && ch->credit == 98304 && ch->local_window == 2097152 - 98304,
               "chan in: no adjust at exactly three maximum packets used (OpenSSH's rule)");
    test_check(axl_ssh_chan_inbound(ch, 1, false, true, &adj) == AXL_OK
                   && adj == 98305 && ch->local_window == 2097152 && ch->credit == 0,
               "chan in: one byte past three maximum packets -> adjust 98305 restores 2 MB");
    ok = true;
    for (int i = 0; i < 8; i++) {
        uint32_t want = i % 4 == 3 ? 131072u : 0u;
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK
             && adj == want;
    }
    test_check(ok && ch->local_window == 2097152 && ch->credit == 0,
               "chan in: a steady stream of 32 KB packets draws 128 KB every fourth packet");
    test_check(axl_ssh_chan_inbound(ch, 10, true, true, &adj) == AXL_SSH_CHAN_IN_DISCARD
                   && ch->credit == 10 && ch->local_window == 2097142,
               "chan in: extended data is counted and discarded");

    /* The grace: a peer that ignores the window gets 10% (209715) more.
       Reachable only where the window can close, so in ON_CONSUME -- with
       nothing unconsumed, so the UNCONSUMED_MAX bound is far off and the
       grace alone is what refuses the next byte. */
    ch->on_consume   = true;
    ch->local_window = 0;
    ch->credit       = 0;
    ch->unconsumed   = 0;
    ok               = true;
    for (int i = 0; i < 6; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK
             && adj == 0;
    }
    test_check(ok && axl_ssh_chan_inbound(ch, 13107, false, true, &adj) == AXL_OK
                   && ch->exceeded == 209715 && ch->local_window == 0
                   && ch->unconsumed == 209715 && adj == 0,
               "chan in: exactly 209715 past the window is within the grace");
    test_check(axl_ssh_chan_inbound(ch, 1, false, true, &adj) == AXL_ERR,
               "chan in: one byte beyond the grace is AXL_ERR");

    /* After the peer's EOF. */
    axl_ssh_chan_free(ch, &b);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_start(ch, &adj);
    ch->eof_in = true;
    test_check(axl_ssh_chan_inbound(ch, 10, false, true, &adj) == AXL_SSH_CHAN_IN_DISCARD
                   && ch->credit == 10 && ch->local_window == 2097142,
               "chan in: data after EOF is counted and discarded");
    ch->local_window = 1048576;
    ch->credit       = 1048576;
    test_check(axl_ssh_chan_inbound(ch, 1, false, true, &adj) == AXL_SSH_CHAN_IN_DISCARD
                   && adj == 1048577 && ch->local_window == 2097152
                   && ch->credit == 0,
               "chan in: after EOF the window is still re-opened (no deadlock)");
    test_check(axl_ssh_chan_inbound(ch, 10, true, true, &adj) == AXL_ERR,
               "chan in: extended data after EOF is AXL_ERR");

    /* Retired (we closed first): counted, ignored, never fatal. */
    axl_ssh_chan_free(ch, &b);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_start(ch, &adj);
    axl_ssh_chan_retire(ch, &b);
    ch->local_window = 0;
    ok               = true;
    for (int i = 0; i < 7; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj)
                       == AXL_SSH_CHAN_IN_DISCARD && adj == 0;
    }
    test_check(ok && ch->credit == 229376,
               "chan in: a retired channel counts data past the grace, never fatal, no adjust");
    ch->eof_in = true;
    test_check(axl_ssh_chan_inbound(ch, 10, true, true, &adj) == AXL_SSH_CHAN_IN_DISCARD,
               "chan in: a retired channel ignores extended data after EOF too");
    chan_free_all(tab, &b);
}

/* ON_CONSUME, as pure bookkeeping: delivered bytes wait as `unconsumed`
   until consume() moves them to `credit`; every byte the library discards
   itself is credit at once; take_adjust() grants min(credit, room) and only
   once the window is more than three maximum packets below its size;
   UNCONSUMED_MAX is a direct bound that an empty packet cannot reset. */
static void
test_ssh_chan_consume(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    uint32_t         adj = 99;
    const uint32_t   w   = AXL_SSH_CHANNEL_WINDOW_SIZE;
    axl_memset(tab, 0, sizeof tab);
    axl_ssh_chan_budget_init(&b, 1000, 1000, 0);

    test_check(AXL_SSH_CHANNEL_WINDOW_SIZE == 2097152u
                   && AXL_SSH_CHANNEL_UNCONSUMED_MAX == 2306867u
                   && AXL_SSH_CHAN_LOCAL_WINDOW == AXL_SSH_CHANNEL_WINDOW_SIZE,
               "chan consume: the window is 2 MB, the unconsumed bound 2 MB + 10%");

    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_start(ch, &adj);
    axl_ssh_chan_set_mode(ch, true);
    test_check(ch->on_consume && ch->unconsumed == 0 && ch->credit == 0,
               "chan consume: set_mode(true) holds nothing yet");
    bool ok = true;
    for (unsigned i = 0; i < 64; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK
             && adj == 0;
    }
    test_check(ok && ch->unconsumed == w && ch->credit == 0
                   && ch->local_window == 0 && !axl_ssh_chan_adjust_due(ch),
               "chan consume: a full window delivered, none consumed: no adjust, window 0");

    test_check(axl_ssh_chan_consume(ch, (size_t)w + 1u) == AXL_INVALID
                   && ch->unconsumed == w && ch->credit == 0,
               "chan consume: one byte more than delivered is INVALID, nothing moves");
    test_check(axl_ssh_chan_consume(ch, SIZE_MAX) == AXL_INVALID
                   && ch->unconsumed == w && ch->credit == 0,
               "chan consume: SIZE_MAX is INVALID, not narrowed to a u32");
    test_check(axl_ssh_chan_consume(ch, (size_t)UINT32_MAX + 1u + 5u)
                   == AXL_INVALID && ch->unconsumed == w,
               "chan consume: 2^32 + 5 is INVALID (would narrow to 5)");
    test_check(axl_ssh_chan_consume(ch, 0) == AXL_OK && ch->unconsumed == w
                   && ch->credit == 0,
               "chan consume: 0 is OK and changes nothing");

    for (unsigned i = 0; i < 10; i++) {
        ok = ok && axl_ssh_chan_consume(ch, 1000) == AXL_OK;
    }
    test_check(ok && ch->unconsumed == w - 10000u && ch->credit == 10000
                   && axl_ssh_chan_adjust_due(ch),
               "chan consume: ten reports of 1000 are 10000 credit, an adjust due");
    test_check(axl_ssh_chan_take_adjust(ch) == 10000 && ch->credit == 0
                   && ch->local_window == 10000 && !axl_ssh_chan_adjust_due(ch)
                   && axl_ssh_chan_take_adjust(ch) == 0,
               "chan consume: ONE adjust of 10000 for the ten reports, then none");

    /* Discarded bytes are the library's to credit, in ON_CONSUME too --
       and with the window more than three maximum packets below its size,
       each is granted straight back. */
    test_check(axl_ssh_chan_inbound(ch, 100, true, true, &adj)
                   == AXL_SSH_CHAN_IN_DISCARD
                   && ch->unconsumed == w - 10000u && adj == 100
                   && ch->credit == 0 && ch->local_window == 10000,
               "chan consume: extended data is credit, never unconsumed");
    test_check(axl_ssh_chan_inbound(ch, 200, false, false, &adj) == AXL_OK
                   && ch->unconsumed == w - 10000u && adj == 200
                   && ch->credit == 0 && ch->local_window == 10000,
               "chan consume: data no hook will see is credit, never unconsumed");

    /* Switching back credits everything owed, and a late report is INVALID. */
    axl_ssh_chan_set_mode(ch, false);
    test_check(!ch->on_consume && ch->unconsumed == 0
                   && ch->credit == w - 10000u,
               "chan consume: back to ON_ARRIVAL, every unconsumed byte is credit");
    test_check(axl_ssh_chan_consume(ch, 1) == AXL_INVALID,
               "chan consume: after switching back, a report of 1 is INVALID");
    test_check(axl_ssh_chan_take_adjust(ch) == w - 10000u
                   && ch->local_window == w && ch->credit == 0,
               "chan consume: one adjust restores the whole window");
    test_check(axl_ssh_chan_inbound(ch, 1, false, true, &adj) == AXL_OK
                   && ch->unconsumed == 0 && ch->credit == 1,
               "chan consume: in ON_ARRIVAL a delivered byte is credit at once");

    /* After the peer's EOF and before the start: never unconsumed. */
    axl_ssh_chan_free(ch, &b);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_set_mode(ch, true);
    test_check(axl_ssh_chan_inbound(ch, 100, false, true, &adj)
                   == AXL_SSH_CHAN_IN_DISCARD
                   && ch->unconsumed == 0 && ch->credit == 0,
               "chan consume: before the start, data is neither unconsumed nor credit");
    axl_ssh_chan_start(ch, &adj);
    ch->eof_in = true;
    test_check(axl_ssh_chan_inbound(ch, 100, false, true, &adj)
                   == AXL_SSH_CHAN_IN_DISCARD
                   && ch->unconsumed == 0 && ch->credit == 100,
               "chan consume: after the peer's EOF, data is credit, never unconsumed");

    /* The direct bound: a full window and the whole grace unconsumed, an
       EMPTY packet (which resets sshd's grace counter), then one byte. */
    axl_ssh_chan_free(ch, &b);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_start(ch, &adj);
    axl_ssh_chan_set_mode(ch, true);
    for (unsigned i = 0; i < 64; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK;
    }
    for (unsigned i = 0; i < 6; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK;
    }
    test_check(ok && axl_ssh_chan_inbound(ch, 13107, false, true, &adj) == AXL_OK
                   && ch->unconsumed == AXL_SSH_CHANNEL_UNCONSUMED_MAX,
               "chan consume: exactly UNCONSUMED_MAX unconsumed is accepted");
    test_check(axl_ssh_chan_inbound(ch, 0, false, true, &adj) == AXL_OK
                   && ch->exceeded == 0,
               "chan consume: an empty packet resets the grace counter (sshd's rule)");
    test_check(axl_ssh_chan_inbound(ch, 1, false, true, &adj) == AXL_ERR,
               "chan consume: one byte past UNCONSUMED_MAX is AXL_ERR despite the reset");

    /* The bound counts only what a hook will hold: the same overrun with no
       data hook is credit, judged by the grace alone. */
    axl_ssh_chan_free(ch, &b);
    ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    axl_ssh_chan_start(ch, &adj);
    axl_ssh_chan_set_mode(ch, true);
    for (unsigned i = 0; i < 64; i++) {
        ok = ok && axl_ssh_chan_inbound(ch, 32768, false, true, &adj) == AXL_OK;
    }
    test_check(ok && axl_ssh_chan_consume(ch, w) == AXL_OK
                   && axl_ssh_chan_take_adjust(ch) == w
                   && ch->local_window == w && ch->unconsumed == 0,
               "chan consume: consuming the whole window grants it back in one adjust");

    /* The cap: credit beyond the room left in the window is a peer's
       overrun, and is cut rather than granted (the window never passes its
       size, so the peer's never passes 2^32 - 1). */
    ch->local_window = w - 100u;
    ch->credit       = w;
    test_check(axl_ssh_chan_adjust_due(ch) == false,
               "chan consume: with no more than three maximum packets owed, no adjust is due");
    ch->local_window = w - 98305u;
    test_check(axl_ssh_chan_adjust_due(ch),
               "chan consume: one byte past three maximum packets below the window, an adjust is due");
    ch->local_window = 100;
    test_check(axl_ssh_chan_take_adjust(ch) == w - 100u
                   && ch->local_window == w && ch->credit == 0,
               "chan consume: an adjust is capped at the room left in the window");

    /* The inline path (inbound, ON_ARRIVAL's per-packet adjust) takes the
       SAME capped grant: an empty packet with overrun credit owed is
       granted only the room left. */
    ch->local_window = 100;
    ch->credit       = w;
    test_check(axl_ssh_chan_inbound(ch, 0, false, true, &adj) == AXL_OK
                   && adj == w - 100u && ch->local_window == w && ch->credit == 0,
               "chan consume: inbound's own adjust is capped at the room left too");

    /* Nothing is granted once our CLOSE is out, on the inline path or the
       flush's -- before the handle is retired, as when a close waits for a
       hook to return. */
    ch->local_window = 0;
    ch->credit       = 1000;
    ch->close_sent   = true;
    test_check(axl_ssh_chan_inbound(ch, 10, false, true, &adj)
                   == AXL_SSH_CHAN_IN_DISCARD && adj == 0
                   && !axl_ssh_chan_adjust_due(ch) && axl_ssh_chan_take_adjust(ch) == 0
                   && ch->local_window == 0,
               "chan consume: after our CLOSE, neither path grants an adjust");
    ch->close_sent = false;

    /* Nothing is granted on a channel we have closed. */
    ch->local_window = 0;
    ch->credit       = 1000;
    axl_ssh_chan_retire(ch, &b);
    test_check(!axl_ssh_chan_adjust_due(ch) && axl_ssh_chan_take_adjust(ch) == 0
                   && ch->local_window == 0,
               "chan consume: a retired channel is never granted an adjust");
    test_check(!axl_ssh_chan_adjust_due(NULL) && axl_ssh_chan_take_adjust(NULL) == 0
                   && axl_ssh_chan_consume(NULL, 1) == AXL_INVALID,
               "chan consume: NULL-safe");
    axl_ssh_chan_set_mode(NULL, true);
    chan_free_all(tab, &b);
}

static void
test_ssh_chan_peer_adjust(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    axl_memset(tab, 0, sizeof tab);
    axl_ssh_chan_budget_init(&b, 1000, 1000, 0);
    AxlSshChanSlot *ch = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);

    ch->peer_window = 100;
    test_check(axl_ssh_chan_peer_adjust(ch, 50) == AXL_OK && ch->peer_window == 150,
               "chan adjust: 100 + 50 = 150");
    test_check(axl_ssh_chan_peer_adjust(ch, UINT32_MAX - 150) == AXL_OK
                   && ch->peer_window == UINT32_MAX,
               "chan adjust: exactly to 2^32-1 is accepted");
    ch->peer_window = 10;
    test_check(axl_ssh_chan_peer_adjust(ch, UINT32_MAX - 9) == AXL_ERR
                   && ch->peer_window == 10,
               "chan adjust: one past 2^32-1 is AXL_ERR, window unchanged");
    axl_ssh_chan_retire(ch, &b);
    test_check(axl_ssh_chan_peer_adjust(ch, UINT32_MAX) == AXL_OK
                   && ch->peer_window == 10,
               "chan adjust: a retired channel ignores it, even an overflowing one");
    test_check(axl_ssh_chan_peer_adjust(NULL, 1) == AXL_INVALID,
               "chan adjust: a NULL channel is AXL_INVALID");
    chan_free_all(tab, &b);
}

static void
test_ssh_chan_free(void)
{
    AxlSshChanSlot   tab[CHAN_TAB];
    AxlSshChanBudget b;
    uint8_t          buf[300];
    axl_memset(tab, 0, sizeof tab);
    axl_memset(buf, 'f', sizeof buf);
    axl_ssh_chan_budget_init(&b, 1000, 1000, 0);

    AxlSshChanSlot *c1 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    AxlSshChanSlot *c2 = axl_ssh_chan_alloc(tab, CHAN_TAB, CHAN_TAB);
    test_check(axl_ssh_chan_write(c1, &b, false, buf, 300) == AXL_OK
                   && axl_ssh_chan_write(c2, &b, true, buf, 200) == AXL_OK
                   && b.queued_total == 500,
               "chan free: two channels hold 500");
    axl_ssh_chan_free(c1, &b);
    test_check(b.queued_total == 200 && c1->id == 0 && c1->out == NULL
                   && c1->queued == 0,
               "chan free: the freed channel's 300 leave the total; the slot is free");
    axl_ssh_chan_retire(c2, &b);
    test_check(b.queued_total == 0 && c2->queued == 0 && c2->retired && c2->id != 0,
               "chan retire: queued output is discarded back to the budget; slot kept");
    axl_ssh_chan_free(c2, &b);
    axl_ssh_chan_free(NULL, &b);
    test_check(b.queued_total == 0 && c2->id == 0, "chan free: a retired slot frees");
}

/* ------------------------------------------------------------------------ */
/* Rekey decisions and staged keys (src/net/axl-ssh-rekey.h)                */
/* ------------------------------------------------------------------------ */

/* The triggers are sshd's ssh_packet_need_rekeying(): STRICTLY more than the
   limit, in either direction, and nothing while an exchange runs. 2^31
   packets cannot be counted one at a time in a unit test, so the counter is
   set one short and the last packet goes through the real count call. */
static void
test_ssh_rekey_due(void)
{
    AxlSshRekey r;

    axl_ssh_rekey_init(&r, 1000);
    test_check(!axl_ssh_rekey_due(&r) && !axl_ssh_rekey_active(&r),
               "rekey due: a fresh state is neither due nor active");
    axl_ssh_rekey_count_out(&r, 600);
    axl_ssh_rekey_count_out(&r, 400);
    test_check(r.bytes_out == 1000 && r.pkts_out == 2,
               "rekey count: two packets out, 1000 bytes");
    test_check(!axl_ssh_rekey_due(&r),
               "rekey due: exactly the limit out is not yet due");
    axl_ssh_rekey_count_out(&r, 1);
    test_check(axl_ssh_rekey_due(&r), "rekey due: one byte past the limit out is due");

    axl_ssh_rekey_init(&r, 1000);
    axl_ssh_rekey_count_in(&r, 1000);
    test_check(r.bytes_in == 1000 && r.pkts_in == 1 && r.bytes_out == 0,
               "rekey count: inbound counts only inbound");
    test_check(!axl_ssh_rekey_due(&r),
               "rekey due: exactly the limit in is not yet due");
    axl_ssh_rekey_count_in(&r, 1);
    test_check(axl_ssh_rekey_due(&r), "rekey due: one byte past the limit in is due");

    axl_ssh_rekey_init(&r, 0);
    r.bytes_out = 1ull << 62;
    r.bytes_in  = 1ull << 62;
    r.pkts_out  = 1;
    test_check(!axl_ssh_rekey_due(&r), "rekey due: limit 0 is never due by bytes");

    r.pkts_out = AXL_SSH_REKEY_PACKETS - 1;
    axl_ssh_rekey_count_out(&r, 16);
    test_check(r.pkts_out == AXL_SSH_REKEY_PACKETS && !axl_ssh_rekey_due(&r),
               "rekey due: exactly 2^31 packets out is not yet due");
    axl_ssh_rekey_count_out(&r, 16);
    test_check(axl_ssh_rekey_due(&r),
               "rekey due: 2^31 + 1 packets out is due, with limit 0");

    axl_ssh_rekey_init(&r, 0);
    r.pkts_in = AXL_SSH_REKEY_PACKETS;
    test_check(!axl_ssh_rekey_due(&r), "rekey due: exactly 2^31 packets in is not yet due");
    axl_ssh_rekey_count_in(&r, 16);
    test_check(axl_ssh_rekey_due(&r),
               "rekey due: 2^31 + 1 packets in is due, with limit 0");

    /* A trigger mid-exchange starts nothing: whichever side opened it. */
    axl_ssh_rekey_init(&r, 10);
    axl_ssh_rekey_count_out(&r, 11);
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK, "rekey due: our KEXINIT accepted");
    test_check(axl_ssh_rekey_active(&r) && !axl_ssh_rekey_due(&r),
               "rekey due: not due while our exchange is active");
    axl_ssh_rekey_init(&r, 10);
    axl_ssh_rekey_count_in(&r, 11);
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK,
               "rekey due: the peer's KEXINIT accepted with no out-param");
    test_check(axl_ssh_rekey_active(&r) && !axl_ssh_rekey_due(&r),
               "rekey due: not due while the peer's exchange is active");

    test_check(!axl_ssh_rekey_due(NULL) && !axl_ssh_rekey_active(NULL),
               "rekey due: NULL is neither due nor active");
    axl_ssh_rekey_init(NULL, 1);
    axl_ssh_rekey_count_out(NULL, 1);
    axl_ssh_rekey_count_in(NULL, 1);
    test_check(axl_ssh_rekey_on_our_kexinit(NULL) == AXL_ERR
                   && axl_ssh_rekey_on_peer_kexinit(NULL, NULL) == AXL_ERR
                   && axl_ssh_rekey_on_our_newkeys(NULL) == AXL_ERR
                   && axl_ssh_rekey_on_peer_newkeys(NULL) == AXL_ERR
                   && !axl_ssh_rekey_in_kex(NULL),
               "rekey events: NULL is AXL_ERR");
}

/* @return true when every message in @a msgs gets @a want from rekey_out. */
static bool
rekey_out_all(
    const AxlSshRekey *r,
    const uint8_t     *msgs,
    size_t             n,
    AxlSshRekeyOut     want
)
{
    for (size_t i = 0; i < n; i++) {
        if (axl_ssh_rekey_out(r, msgs[i]) != want) {
            return false;
        }
    }
    return true;
}

/* @return true when every message in @a msgs gets @a want from in_allowed. */
static bool
rekey_in_all(
    const AxlSshRekey *r,
    const uint8_t     *msgs,
    size_t             n,
    bool               want
)
{
    for (size_t i = 0; i < n; i++) {
        if (axl_ssh_rekey_in_allowed(r, msgs[i]) != want) {
            return false;
        }
    }
    return true;
}

/* Outbound is sshd's ssh_packet_type_is_kex(): between our KEXINIT and our
   NEWKEYS, 1-49 leave except 5, 6, 7; everything else waits. */
static void
test_ssh_rekey_out(void)
{
    static const uint8_t kex[]  = { 1, 2, 3, 4, 8, 20, 21, 30, 31, 49 };
    static const uint8_t held[] = { 0, 5, 6, 7, 50, 80, 93, 94, 96, 97, 98, 255 };
    AxlSshRekey r;

    axl_ssh_rekey_init(&r, 0);
    test_check(rekey_out_all(&r, kex, sizeof kex, AXL_SSH_REKEY_SEND)
                   && rekey_out_all(&r, held, sizeof held, AXL_SSH_REKEY_SEND),
               "rekey out: before our KEXINIT everything is sent");
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK, "rekey out: peer KEXINIT accepted");
    test_check(axl_ssh_rekey_out(&r, 94) == AXL_SSH_REKEY_SEND,
               "rekey out: the peer's KEXINIT alone holds nothing of ours");
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK, "rekey out: our_kexinit accepted");
    test_check(rekey_out_all(&r, kex, sizeof kex, AXL_SSH_REKEY_SEND),
               "rekey out: after our KEXINIT transport and kex messages are sent");
    test_check(rekey_out_all(&r, held, sizeof held, AXL_SSH_REKEY_HOLD),
               "rekey out: after our KEXINIT 0, 5-7 and 50+ are held");
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_OK, "rekey out: our_newkeys accepted");
    test_check(rekey_out_all(&r, kex, sizeof kex, AXL_SSH_REKEY_SEND)
                   && rekey_out_all(&r, held, sizeof held, AXL_SSH_REKEY_SEND),
               "rekey out: after our NEWKEYS everything is sent again");
    test_check(axl_ssh_rekey_out(NULL, 94) == AXL_SSH_REKEY_SEND,
               "rekey out: NULL holds nothing");

    /* is_kex is the same set out() lets through mid-exchange. */
    bool kex_ok = true;
    for (size_t i = 0; i < sizeof kex; i++) {
        kex_ok = kex_ok && axl_ssh_rekey_is_kex(kex[i]);
    }
    for (size_t i = 0; i < sizeof held; i++) {
        kex_ok = kex_ok && !axl_ssh_rekey_is_kex(held[i]);
    }
    test_check(kex_ok, "rekey is_kex: 1-49 but 5-7 are kex; 0, 5-7 and 50+ are not");
}

/* Inbound is sshd's dispatch table during a rekey -- see the header. */
static void
test_ssh_rekey_in(void)
{
    static const uint8_t every[]   = { 0, 1, 5, 6, 7, 20, 21, 22, 30, 50, 94, 255 };
    static const uint8_t allowed[] = { 1, 2, 3, 4, 30, 31, 49,
                                       50, 80, 90, 93, 94, 97, 98, 255 };
    static const uint8_t refused[] = { 0, 5, 6, 7, 8, 19, 20, 22, 29 };
    AxlSshRekey r;

    axl_ssh_rekey_init(&r, 0);
    test_check(rekey_in_all(&r, every, sizeof every, true),
               "rekey in: before any KEXINIT everything is processed");
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK, "rekey in: our_kexinit accepted");
    test_check(rekey_in_all(&r, every, sizeof every, true),
               "rekey in: our KEXINIT alone restricts nothing inbound");
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK, "rekey in: peer KEXINIT accepted");
    test_check(rekey_in_all(&r, allowed, sizeof allowed, true),
               "rekey in: after the peer's KEXINIT 1-4, 30-49 and 50+ are processed");
    test_check(rekey_in_all(&r, refused, sizeof refused, false),
               "rekey in: after the peer's KEXINIT 0, 5-19, a second KEXINIT and"
               " 22-29 are refused");
    test_check(!axl_ssh_rekey_in_allowed(&r, 21),
               "rekey in: the peer's NEWKEYS before ours is refused");
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_OK, "rekey in: our_newkeys accepted");
    test_check(axl_ssh_rekey_in_allowed(&r, 21),
               "rekey in: the peer's NEWKEYS after ours is processed");
    test_check(!axl_ssh_rekey_in_allowed(&r, 5) && !axl_ssh_rekey_in_allowed(&r, 20),
               "rekey in: our NEWKEYS does not lift the inbound restriction");
    test_check(axl_ssh_rekey_on_peer_newkeys(&r) == AXL_OK, "rekey in: peer_newkeys accepted");
    test_check(rekey_in_all(&r, every, sizeof every, true),
               "rekey in: after the peer's NEWKEYS everything is processed again");
    test_check(!axl_ssh_rekey_in_allowed(NULL, 94), "rekey in: NULL processes nothing");
}

/* Each direction's counters restart at THAT direction's NEWKEYS (sshd's
   ssh_set_newkeys), and the exchange ends at the peer's, which can only come
   second. */
static void
test_ssh_rekey_counters(void)
{
    AxlSshRekey r;

    axl_ssh_rekey_init(&r, 100);
    axl_ssh_rekey_count_out(&r, 200);
    axl_ssh_rekey_count_in(&r, 300);
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK
                   && axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK,
               "rekey counters: both KEXINITs accepted");
    axl_ssh_rekey_count_in(&r, 10);
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_OK,
               "rekey counters: our NEWKEYS accepted");
    test_check(r.bytes_out == 0 && r.pkts_out == 0,
               "rekey counters: our NEWKEYS restarts the outbound counters");
    test_check(r.bytes_in == 310 && r.pkts_in == 2,
               "rekey counters: our NEWKEYS leaves the inbound counters");
    test_check(axl_ssh_rekey_active(&r),
               "rekey counters: still active after our NEWKEYS alone");
    axl_ssh_rekey_count_out(&r, 7);
    test_check(axl_ssh_rekey_on_peer_newkeys(&r) == AXL_OK,
               "rekey counters: the peer's NEWKEYS accepted");
    test_check(r.bytes_in == 0 && r.pkts_in == 0,
               "rekey counters: the peer's NEWKEYS restarts the inbound counters");
    test_check(r.bytes_out == 7 && r.pkts_out == 1,
               "rekey counters: the peer's NEWKEYS keeps what the new outbound key"
               " already carried");
    test_check(!axl_ssh_rekey_active(&r) && !r.ours_sent && !r.peers_seen
                   && !r.ours_newkeys,
               "rekey counters: both NEWKEYS end the exchange");
    test_check(r.limit_bytes == 100, "rekey counters: the limit survives an exchange");

    /* A finished exchange leaves the state ready for the next one. */
    axl_ssh_rekey_count_out(&r, 101);
    test_check(axl_ssh_rekey_due(&r),
               "rekey counters: the limit triggers again after an exchange");
}

/* Put @a r mid-exchange: both KEXINITs in, ours first, counters non-zero. */
static void
rekey_start(
    AxlSshRekey *r
)
{
    axl_ssh_rekey_init(r, 100);
    axl_ssh_rekey_count_out(r, 50);
    axl_ssh_rekey_count_in(r, 60);
    test_check(axl_ssh_rekey_on_our_kexinit(r) == AXL_OK
                   && axl_ssh_rekey_on_peer_kexinit(r, NULL) == AXL_OK,
               "rekey events: fixture exchange starts");
}

/* The four events refuse an order sshd would never reach, and a refused
   event changes nothing -- otherwise a stray one leaves a flag set across
   exchanges, and the NEXT exchange sends channel data mid-window or
   processes everything inbound. */
static void
test_ssh_rekey_events(void)
{
    AxlSshRekey r;
    AxlSshRekey before;
    bool        send_ours = false;

    /* Who sends our KEXINIT: the peer starting it means we answer now. */
    axl_ssh_rekey_init(&r, 0);
    send_ours = false;
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, &send_ours) == AXL_OK && send_ours,
               "rekey events: a peer-started exchange asks for our KEXINIT");
    axl_ssh_rekey_init(&r, 0);
    send_ours = true;
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK
                   && axl_ssh_rekey_on_peer_kexinit(&r, &send_ours) == AXL_OK
                   && !send_ours,
               "rekey events: an exchange we started does not ask for a second KEXINIT");

    /* in_kex: from the peer's KEXINIT until its NEWKEYS, and only then. */
    axl_ssh_rekey_init(&r, 0);
    test_check(!axl_ssh_rekey_in_kex(&r), "rekey in_kex: false before any KEXINIT");
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK && !axl_ssh_rekey_in_kex(&r),
               "rekey in_kex: false on our KEXINIT alone");
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK
                   && axl_ssh_rekey_in_kex(&r),
               "rekey in_kex: true after the peer's KEXINIT");
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_OK && axl_ssh_rekey_in_kex(&r),
               "rekey in_kex: still true after our NEWKEYS");
    test_check(axl_ssh_rekey_on_peer_newkeys(&r) == AXL_OK && !axl_ssh_rekey_in_kex(&r),
               "rekey in_kex: false after the peer's NEWKEYS");

    /* A second KEXINIT from either side in one exchange. */
    rekey_start(&r);
    axl_memcpy(&before, &r, sizeof before);
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: a second KEXINIT of ours is refused, nothing changed");
    send_ours = true;
    test_check(axl_ssh_rekey_on_peer_kexinit(&r, &send_ours) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: a second KEXINIT of the peer's is refused, nothing"
               " changed");

    /* Our NEWKEYS needs both KEXINITs and only one NEWKEYS of ours. */
    axl_ssh_rekey_init(&r, 100);
    axl_ssh_rekey_count_out(&r, 50);
    axl_memcpy(&before, &r, sizeof before);
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: our NEWKEYS outside an exchange is refused, counters"
               " kept");
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK
                   && axl_ssh_rekey_on_our_newkeys(&r) == AXL_ERR
                   && !r.ours_newkeys && r.pkts_out == 1,
               "rekey events: our NEWKEYS before the peer's KEXINIT is refused");
    rekey_start(&r);
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_OK, "rekey events: our NEWKEYS");
    axl_ssh_rekey_count_out(&r, 9);
    axl_memcpy(&before, &r, sizeof before);
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: a second NEWKEYS of ours is refused, counters kept");

    /* The peer's NEWKEYS needs its KEXINIT and our NEWKEYS. */
    axl_ssh_rekey_init(&r, 100);
    axl_ssh_rekey_count_in(&r, 60);
    axl_memcpy(&before, &r, sizeof before);
    test_check(axl_ssh_rekey_on_peer_newkeys(&r) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: the peer's NEWKEYS outside an exchange is refused,"
               " counters kept");
    rekey_start(&r);
    axl_memcpy(&before, &r, sizeof before);
    test_check(axl_ssh_rekey_on_peer_newkeys(&r) == AXL_ERR
                   && axl_memcmp(&r, &before, sizeof r) == 0,
               "rekey events: the peer's NEWKEYS before ours is refused, nothing"
               " changed");

    /* The defect the checks exist for: a stray NEWKEYS between exchanges
       must not leave the next one sending channel data mid-window or
       processing everything inbound. */
    axl_ssh_rekey_init(&r, 0);
    test_check(axl_ssh_rekey_on_our_newkeys(&r) == AXL_ERR
                   && axl_ssh_rekey_on_peer_newkeys(&r) == AXL_ERR,
               "rekey events: stray NEWKEYS between exchanges are refused");
    test_check(axl_ssh_rekey_on_our_kexinit(&r) == AXL_OK
                   && axl_ssh_rekey_on_peer_kexinit(&r, NULL) == AXL_OK
                   && axl_ssh_rekey_out(&r, 94) == AXL_SSH_REKEY_HOLD
                   && !axl_ssh_rekey_in_allowed(&r, 5),
               "rekey events: after stray NEWKEYS the next exchange still holds 94"
               " and refuses 5");
}

/* @return true when @a len bytes at @a hay contain @a needle anywhere. */
static bool
rekey_bytes_contain(
    const uint8_t *hay,
    size_t         len,
    const uint8_t *needle,
    size_t         nlen
)
{
    for (size_t i = 0; i + nlen <= len; i++) {
        if (axl_memcmp(hay + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* @return true when @a len bytes at @a p are all zero. */
static bool
rekey_all_zero(
    const void *p,
    size_t      len
)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < len; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

/* @return true when @a a opens @a pkt (sealed at sequence 5). */
static bool
rekey_opens(
    const AxlSshAead *a,
    AxlString        *pkt
)
{
    uint8_t  plain[64];
    uint32_t plen     = 0;
    size_t   consumed = 0;
    return axl_ssh_aead_open(a, 5, (const uint8_t *)axl_string_data(pkt),
                             axl_string_len(pkt), &consumed, plain,
                             sizeof plain, &plen) == AXL_OK;
}

/* Staged keys: a new key changes nothing until the matching NEWKEYS, and the
   key it replaces does not outlive the switch. */
static void
test_ssh_dirkey(void)
{
    static const uint8_t msg[9] = { 94, 'r', 'e', 'k', 'e', 'y', 'e', 'd', '!' };
    uint8_t old_key[AXL_SSH_AEAD_KEY_LEN];
    uint8_t new_key[AXL_SSH_AEAD_KEY_LEN];
    for (size_t i = 0; i < sizeof old_key; i++) {
        old_key[i] = (uint8_t)(i + 1);
        new_key[i] = (uint8_t)(0x80 + i);
    }

    /* One packet sealed under each key, by an AEAD the dirkey never sees. */
    AxlSshAead    a_old;
    AxlSshAead    a_new;
    AxlSshRngPool pool;
    axl_ssh_rng_pool_init(&pool);
    AxlString *p_old = axl_string_new("");
    AxlString *p_new = axl_string_new("");
    bool sealed = p_old != NULL && p_new != NULL
                  && axl_ssh_aead_set_key(&a_old, old_key) == AXL_OK
                  && axl_ssh_aead_set_key(&a_new, new_key) == AXL_OK
                  && axl_ssh_aead_seal(&a_old, 5, msg, sizeof msg, &pool, p_old)
                         == AXL_OK
                  && axl_ssh_aead_seal(&a_new, 5, msg, sizeof msg, &pool, p_new)
                         == AXL_OK;
    test_check(sealed, "dirkey: fixture packets seal");
    if (!sealed) {
        axl_string_free(p_old);
        axl_string_free(p_new);
        axl_ssh_rng_pool_clear(&pool);
        return;
    }

    AxlSshDirKey k;
    axl_memset(&k, 0, sizeof k);
    test_check(axl_ssh_dirkey_switch(&k) == AXL_ERR,
               "dirkey: switch with nothing staged is AXL_ERR");
    test_check(axl_ssh_dirkey_stage(&k, old_key) == AXL_OK
                   && axl_ssh_dirkey_switch(&k) == AXL_OK
                   && rekey_opens(&k.cur, p_old),
               "dirkey: stage then switch installs a key");

    test_check(axl_ssh_dirkey_stage(&k, new_key) == AXL_OK && k.has_next,
               "dirkey: stage accepts the new key");
    test_check(rekey_opens(&k.cur, p_old),
               "dirkey: after stage the current key still opens an old-key packet");
    test_check(!rekey_opens(&k.cur, p_new),
               "dirkey: after stage the current key does not open a new-key packet");
    test_check(axl_ssh_dirkey_stage(&k, old_key) == AXL_ERR
                   && axl_memcmp(k.next.k_payload, new_key, 32) == 0
                   && axl_memcmp(k.next.k_length, new_key + 32, 32) == 0,
               "dirkey: a second stage is AXL_ERR and keeps the staged key");

    test_check(axl_ssh_dirkey_switch(&k) == AXL_OK && !k.has_next,
               "dirkey: switch consumes the staged key");
    test_check(rekey_opens(&k.cur, p_new) && !rekey_opens(&k.cur, p_old),
               "dirkey: after switch the current key opens new-key packets only");
    test_check(!rekey_bytes_contain((const uint8_t *)&k, sizeof k, old_key, 32)
                   && !rekey_bytes_contain((const uint8_t *)&k, sizeof k,
                                           old_key + 32, 32),
               "dirkey: no half of the old key survives the switch");
    test_check(rekey_all_zero(&k.next, sizeof k.next),
               "dirkey: the staged copy is scrubbed by the switch");
    test_check(axl_ssh_dirkey_switch(&k) == AXL_ERR && rekey_opens(&k.cur, p_new),
               "dirkey: a second switch is AXL_ERR and keeps the current key");

    test_check(axl_ssh_dirkey_stage(&k, old_key) == AXL_OK, "dirkey: restage");
    axl_ssh_dirkey_clear(&k);
    test_check(rekey_all_zero(&k, sizeof k), "dirkey: clear scrubs both keys");

    test_check(axl_ssh_dirkey_stage(NULL, new_key) == AXL_ERR
                   && axl_ssh_dirkey_stage(&k, NULL) == AXL_ERR
                   && axl_ssh_dirkey_switch(NULL) == AXL_ERR && !k.has_next,
               "dirkey: NULL arguments are AXL_ERR");
    axl_ssh_dirkey_clear(NULL);

    axl_ssh_secure_zero(&a_old, sizeof a_old);
    axl_ssh_secure_zero(&a_new, sizeof a_new);
    axl_ssh_secure_zero(old_key, sizeof old_key);
    axl_ssh_secure_zero(new_key, sizeof new_key);
    axl_ssh_rng_pool_clear(&pool);
    axl_string_free(p_old);
    axl_string_free(p_new);
}

/* The KEXINIT a rekey sends: OpenSSH PROTOCOL section 1.10 makes the strict
   pseudo-names valid ONLY in the initial KEXINIT, so a rekey's kex list is the
   real method alone. Strict KEX itself stays in force (the server's
   strict_kex is never cleared); only the advertisement goes. */
static void
test_ssh_kexinit_rekey(void)
{
    AxlString *b = axl_string_new("");
    if (b == NULL || axl_ssh_kexinit_build(b, false) != AXL_OK) {
        test_check(false, "ssh rekey kexinit: build");
        axl_string_free(b);
        return;
    }
    const uint8_t *d   = (const uint8_t *)axl_string_data(b);
    size_t         len = axl_string_len(b);

    /* The kex name-list is the first one, right after the 17-byte header:
       read it exactly rather than searching for an absence. */
    size_t         off = 17;
    const uint8_t *kex = NULL;
    uint32_t       n   = 0;
    static const char WANT[] = "curve25519-sha256";
    test_check(d[0] == AXL_SSH_MSG_KEXINIT
                   && axl_ssh_get_string(d, len, &off, &kex, &n) == AXL_OK
                   && n == sizeof WANT - 1
                   && axl_memcmp(kex, WANT, sizeof WANT - 1) == 0,
               "ssh rekey kexinit: the kex list is exactly curve25519-sha256");
    AxlSshKexinitPeer self = { .guessed_wrong = true, .strict_kex = true };
    test_check(axl_ssh_kexinit_select(d, len, &self) == AXL_OK
                   && !self.guessed_wrong && !self.strict_kex,
               "ssh rekey kexinit: our own rekey offer selects, no guess");
    axl_string_free(b);

    /* CONTROL: the initial build of the same function still carries it, so
       the check above is about the flag and not about a broken build. */
    AxlString *i = axl_string_new("");
    off = 17;
    static const char INIT[] = "curve25519-sha256,kex-strict-s-v00@openssh.com";
    test_check(i != NULL && axl_ssh_kexinit_build(i, true) == AXL_OK
                   && axl_ssh_get_string((const uint8_t *)axl_string_data(i),
                                         axl_string_len(i), &off, &kex, &n)
                          == AXL_OK
                   && n == sizeof INIT - 1
                   && axl_memcmp(kex, INIT, sizeof INIT - 1) == 0,
               "ssh rekey kexinit: the initial build still offers kex-strict-s");
    axl_string_free(i);
}

/* axl_ssh_server_set_rekey_limit. What the limit DOES is
   test-ssh-rekey-qemu.sh's (a server-initiated rekey at 64 KB); here, the
   argument rules and that the value reaches the server. */
static void
test_ssh_rekey_limit_setter(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "rekey limit: loop");
    if (loop == NULL) {
        test_skip_n(8, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2264);
    test_check(s != NULL, "rekey limit: server created");
    if (s == NULL) {
        test_skip_n(7, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    uint64_t v = 0;
    test_check(axl_ssh_server_rekey_limit(s, &v) == AXL_OK
                   && v == AXL_SSH_REKEY_LIMIT_DEFAULT
                   && AXL_SSH_REKEY_LIMIT_DEFAULT == 1073741824ull,
               "rekey limit: a new server holds the 1 GiB default");
    test_check(axl_ssh_server_set_rekey_limit(NULL, 4096) == AXL_INVALID,
               "rekey limit: NULL server is invalid");
    v = 1;
    test_check(axl_ssh_server_set_rekey_limit(s, 0) == AXL_OK
                   && axl_ssh_server_rekey_limit(s, &v) == AXL_OK && v == 0,
               "rekey limit: 0 (never by bytes) is accepted and stored");
    test_check(axl_ssh_server_set_rekey_limit(s, 65536) == AXL_OK
                   && axl_ssh_server_rekey_limit(s, &v) == AXL_OK
                   && v == 65536,
               "rekey limit: 65536 is accepted and stored");
    /* sshd's floor (servconf.c, sRekeyLimit: "too small" under 16): every
       packet would otherwise cost an ECDH and a signature. */
    test_check(axl_ssh_server_set_rekey_limit(s, 1) == AXL_INVALID
                   && axl_ssh_server_set_rekey_limit(s, 15) == AXL_INVALID
                   && axl_ssh_server_rekey_limit(s, &v) == AXL_OK
                   && v == 65536,
               "rekey limit: 1 and 15 are refused, the limit unchanged");
    test_check(axl_ssh_server_set_rekey_limit(s, 16) == AXL_OK
                   && axl_ssh_server_rekey_limit(s, &v) == AXL_OK && v == 16,
               "rekey limit: 16, sshd's floor, is accepted");
    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_rekey_limit(s, 4096) == AXL_ERR
                   && axl_ssh_server_rekey_limit(s, &v) == AXL_ERR,
               "rekey limit: refused on a freed server");
    axl_loop_free(loop);
}

/* "In use" (sshd's channel_still_open) and the probe target (sshd's
   channel_find_open) over a channel table: two different sets. */
static void
test_ssh_idle_in_use(void)
{
    AxlSshChanSlot t[4];

    axl_memset(t, 0, sizeof t);
    test_check(!axl_ssh_idle_in_use(t, 4u, 0u),
               "idle in use: an empty table is not");
    t[1].id = 7u;
    test_check(!axl_ssh_idle_in_use(t, 4u, 0u),
               "idle in use: an opened, never started (larval) slot is not");
    test_check(axl_ssh_idle_in_use(t, 4u, 1u),
               "idle in use: a channel hook on the stack is, even with only"
               " a larval slot");
    t[2].id      = 8u;
    t[2].started = true;
    t[2].close_in  = true;
    t[2].close_sent = true;
    test_check(axl_ssh_idle_in_use(t, 4u, 0u),
               "idle in use: a started slot is, closing on both sides,"
               " until freed");
    axl_memset(&t[2], 0, sizeof t[2]);
    t[3].started = true;   /* a freed slot (id 0) never counts */
    test_check(!axl_ssh_idle_in_use(t, 4u, 0u),
               "idle in use: a freed slot is not, whatever its flags");
}

/* The reaper's idle pass skips what is not a live authenticated
   connection: a connection disconnect() marked to end stays listed and
   authenticated until the reply timer frees it. */
static void
test_ssh_idle_scans(void)
{
    test_check(axl_ssh_idle_scans(true, false, false),
               "idle scans: a live authenticated connection is ticked");
    test_check(!axl_ssh_idle_scans(false, false, false),
               "idle scans: an unauthenticated one is not");
    test_check(!axl_ssh_idle_scans(true, true, false),
               "idle scans: a drop conn_drop deferred is not");
    test_check(!axl_ssh_idle_scans(true, false, true),
               "idle scans: one already marked to end is not");
}

static void
test_ssh_idle_probe_slot(void)
{
    AxlSshChanSlot t[4];

    axl_memset(t, 0, sizeof t);
    test_check(axl_ssh_idle_probe_slot(t, 4u) == -1,
               "idle probe slot: none allocated, -1 (GLOBAL_REQUEST)");
    /* OpenSSH answers no request on a channel it has sent CLOSE on
       (clientloop.c:1998), and sends its own the moment ours arrives: a
       slot closing on EITHER side is never the target. */
    t[0].id        = 5u;
    t[0].close_sent = true;
    test_check(axl_ssh_idle_probe_slot(t, 4u) == -1,
               "idle probe slot: our CLOSE sent -> not a target, -1 (GLOBAL_REQUEST)");
    t[1].id       = 9u;
    t[1].close_in = true;
    test_check(axl_ssh_idle_probe_slot(t, 4u) == -1,
               "idle probe slot: the peer's CLOSE in -> not a target either, -1");
    t[2].id = 6u;   /* larval: still a target, as sshd's LARVAL is */
    test_check(axl_ssh_idle_probe_slot(t, 4u) == 2,
               "idle probe slot: the one slot neither side has closed");
}

/* axl_ssh_server_set_unused_timeout and axl_ssh_server_set_client_alive:
   the defaults a new server holds, the argument rules, that a value reaches
   the server, and the refusal once it listens (a REAL bind, for the reason
   test_ssh_login_grace_api records). What the settings DO is
   lib/ssh-idle-cases.sh's (the test-ssh-idle-*-qemu.sh suites). */
static bool
idle_cfg_is(AxlSshServer *s, uint32_t unused, uint32_t alive, uint32_t max)
{
    uint32_t u = 0xffffffffu;
    uint32_t a = 0xffffffffu;
    uint32_t m = 0xffffffffu;

    return axl_ssh_server_idle_config(s, &u, &a, &m) == AXL_OK
           && u == unused && a == alive && m == max;
}

static void
test_ssh_idle_setters(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "idle setters: loop");
    if (loop == NULL) {
        test_skip_n(15, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2265);
    test_check(s != NULL, "idle setters: server created");
    if (s == NULL) {
        test_skip_n(14, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    test_check(AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC == 300u
                   && AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC == 60u
                   && AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX == 3u
                   && AXL_SSH_IDLE_MAX_SEC == 2592000u,
               "idle setters: the defaults are 300 s, 60 s x 3; the cap 30 days");
    test_check(idle_cfg_is(s, 300u, 60u, 3u),
               "idle setters: a new server holds 300 / 60 / 3");
    test_check(axl_ssh_server_set_unused_timeout(NULL, 10u) == AXL_INVALID,
               "idle setters: unused timeout refuses a NULL server");
    test_check(axl_ssh_server_set_client_alive(NULL, 10u, 1u) == AXL_INVALID,
               "idle setters: client alive refuses a NULL server");
    test_check(axl_ssh_server_set_unused_timeout(s, AXL_SSH_IDLE_MAX_SEC + 1u)
                   == AXL_INVALID
                   && idle_cfg_is(s, 300u, 60u, 3u),
               "idle setters: unused timeout refuses one second over the cap,"
               " unchanged");
    test_check(axl_ssh_server_set_client_alive(s, AXL_SSH_IDLE_MAX_SEC + 1u,
                                               1u) == AXL_INVALID
                   && idle_cfg_is(s, 300u, 60u, 3u),
               "idle setters: client alive refuses an interval one second"
               " over the cap, unchanged");
    test_check(axl_ssh_server_set_unused_timeout(s, AXL_SSH_IDLE_MAX_SEC)
                   == AXL_OK
                   && axl_ssh_server_set_client_alive(s, AXL_SSH_IDLE_MAX_SEC,
                                                      0xffffffffu) == AXL_OK
                   && idle_cfg_is(s, AXL_SSH_IDLE_MAX_SEC, AXL_SSH_IDLE_MAX_SEC,
                                  0xffffffffu),
               "idle setters: exactly the cap, and any count, are stored");
    test_check(axl_ssh_server_set_unused_timeout(s, 0u) == AXL_OK
                   && axl_ssh_server_set_client_alive(s, 0u, 0u) == AXL_OK
                   && idle_cfg_is(s, 0u, 0u, 0u),
               "idle setters: 0 (off) is accepted for all three");
    test_check(axl_ssh_server_set_unused_timeout(s, 3u) == AXL_OK
                   && axl_ssh_server_set_client_alive(s, 2u, 2u) == AXL_OK
                   && idle_cfg_is(s, 3u, 2u, 2u),
               "idle setters: 3 and 2 x 2 are stored");
    axl_ssh_server_free(s);
    test_check(axl_ssh_server_set_unused_timeout(s, 3u) == AXL_ERR
                   && axl_ssh_server_set_client_alive(s, 2u, 2u) == AXL_ERR,
               "idle setters: both refused on a freed server");

    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live = axl_ssh_server_new(loop, 2266);
    test_check(live != NULL, "idle setters: second server created");
    if (live == NULL) {
        test_skip_n(3, "no server (Ed25519 provider or RNG unavailable)");
        axl_loop_free(loop);
        return;
    }
    char *key = test_ssh_gen_authkey_line();
    bool ready = key != NULL
                 && axl_ssh_server_add_authorized_key(live, key) == AXL_OK
                 && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(key);
    if (ready) {
        test_check(axl_ssh_server_set_unused_timeout(live, 10u) == AXL_ERR,
                   "idle setters: unused timeout refused once listening");
        test_check(axl_ssh_server_set_client_alive(live, 10u, 1u) == AXL_ERR,
                   "idle setters: client alive refused once listening");
        test_check(idle_cfg_is(live, 300u, 60u, 3u),
                   "idle setters: ...and the defaults are still in force");
    } else {
        test_skip_n(3, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }
    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// P5: the provider-bound host-key functions and axl_ssh_server_set_host_key
// ---------------------------------------------------------------------------

/* The host-key test point this binary arms, or NULL. The library's
   definition is weak and always false; this strong one lets a test fail the
   variable read, the write, or the read-back on demand. */
static const char *g_hk_fault;

bool
_axl_ssh_host_key_test_point(const char *point);

bool
_axl_ssh_host_key_test_point(const char *point)
{
    return g_hk_fault != NULL && axl_strcmp(point, g_hk_fault) == 0;
}

static const AxlGuid HK_TEST_GUID = AXL_SSH_HOST_KEY_GUID;

static void
hk_test_record(AxlSshHostKeyRecord *rec)
{
    static const uint8_t prefix[16] = {
        0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
        0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20
    };
    axl_memcpy(rec->pkcs8, prefix, sizeof prefix);
    axl_memcpy(rec->pkcs8 + sizeof prefix, HK_ED_SEED, sizeof HK_ED_SEED);
    axl_memcpy(rec->public_key, HK_ED_PUBLIC, sizeof HK_ED_PUBLIC);
}

/* The bytes now stored under the host-key name (axl_free them), or NULL. */
static void *
hk_test_stored(size_t *size)
{
    void *data = NULL;
    *size = 0;
    if (axl_var_read(AXL_SSH_HOST_KEY_VAR, &HK_TEST_GUID, NULL, &data, size)
            != AXL_OK) {
        return NULL;
    }
    return data;
}

static bool
hk_why_is(const char *why, const char *expect)
{
    return why != NULL && axl_strcmp(why, expect) == 0;
}

static bool
hk_key_public_is(const AxlPkKey *key, const uint8_t *want)
{
    uint8_t pub[32];
    size_t  n = sizeof pub;
    return key != NULL && axl_pk_key_get_raw_public(key, pub, &n) == AXL_OK
           && n == sizeof pub && axl_memcmp(pub, want, sizeof pub) == 0;
}

static void
test_ssh_host_key_load(void)
{
    AxlSshHostKeyRecord rec;
    AxlPkKey           *key = NULL;
    const char         *why = NULL;

    /* T1's control: AxlTestSshHostKey reads this false. */
    test_check(axl_pk_alg_available(AXL_PK_ED25519),
               "host key: an image that loads keys links the provider");

    hk_test_record(&rec);
    test_check(axl_ssh_host_key_load(&rec, &key, &why) == AXL_OK
               && hk_key_public_is(key, HK_ED_PUBLIC),
               "host key load: ssh-keygen's key loads as itself");
    axl_pk_key_free(key);
    key = NULL;

    rec.public_key[31] ^= 1;
    test_check(axl_ssh_host_key_load(&rec, &key, &why) == AXL_ERR && key == NULL
               && hk_why_is(why, "the private key does not match its public key"),
               "host key load: a public key the seed does not derive is refused");

    hk_test_record(&rec);
    rec.pkcs8[47] ^= 1;   /* a flipped bit in the seed */
    test_check(axl_ssh_host_key_load(&rec, &key, &why) == AXL_ERR && key == NULL
               && hk_why_is(why, "the private key does not match its public key"),
               "host key load: a flipped seed bit is refused, not loaded as"
               " another key");

    hk_test_record(&rec);
    rec.pkcs8[9] ^= 1;    /* the algorithm OID */
    test_check(axl_ssh_host_key_load(&rec, &key, &why) == AXL_ERR && key == NULL
               && hk_why_is(why, "the private key is not a usable Ed25519 key"),
               "host key load: a non-Ed25519 PKCS#8 is refused");

    test_check(axl_ssh_host_key_load(NULL, &key, &why) == AXL_INVALID
               && axl_ssh_host_key_load(&rec, NULL, &why) == AXL_INVALID,
               "host key load: NULL arguments refused");
    axl_ssh_host_key_record_clear(&rec);
}

static void
test_ssh_host_key_record_from_key(void)
{
    AxlSshHostKeyRecord rec;
    AxlPkKey           *back = NULL;
    AxlPkKey           *k    = axl_pk_key_new(AXL_PK_ED25519);
    uint8_t             pub[32];
    size_t              n = sizeof pub;

    test_check(k != NULL && axl_pk_key_get_raw_public(k, pub, &n) == AXL_OK,
               "record_from_key: a generated key");
    test_check(k != NULL && axl_ssh_host_key_record_from_key(k, &rec) == AXL_OK
               && axl_memcmp(rec.public_key, pub, sizeof pub) == 0
               && axl_ssh_host_key_load(&rec, &back, NULL) == AXL_OK
               && hk_key_public_is(back, pub),
               "record_from_key: the record loads back as the same key");
    axl_ssh_host_key_record_clear(&rec);
    axl_pk_key_free(back);
    axl_pk_key_free(k);

    AxlPkKey *pub_only = axl_pk_key_from_raw_public(AXL_PK_ED25519, HK_ED_PUBLIC,
                                                    sizeof HK_ED_PUBLIC);
    axl_memset(&rec, 0xA5, sizeof rec);
    bool zero = true;
    int  rc   = axl_ssh_host_key_record_from_key(pub_only, &rec);
    for (size_t i = 0; i < sizeof rec; i++) {
        zero = zero && ((const uint8_t *)&rec)[i] == 0;
    }
    test_check(pub_only != NULL && rc == AXL_INVALID && zero,
               "record_from_key: a public-only key is refused, record zeroed");
    axl_pk_key_free(pub_only);
    test_check(axl_ssh_host_key_record_from_key(NULL, &rec) == AXL_INVALID,
               "record_from_key: NULL key refused");
}

static void
test_ssh_host_key_nv_load_or_create(void)
{
    AxlPkKey           *key    = NULL;
    AxlSshHostKeyOrigin origin = AXL_SSH_HOST_KEY_LOADED;
    const char         *why    = NULL;
    AxlSshHostKeyRecord rec;
    uint8_t             first[32];
    size_t              n = sizeof first;

    test_check(axl_ssh_host_key_nv_delete() == AXL_OK,
               "load_or_create: start with no stored key");

    /* First start: generated and stored. */
    test_check(axl_ssh_host_key_nv_load_or_create(&key, &origin, &why) == AXL_OK
               && origin == AXL_SSH_HOST_KEY_GENERATED && key != NULL
               && axl_pk_key_get_raw_public(key, first, &n) == AXL_OK,
               "load_or_create: no stored key -> GENERATED");
    axl_pk_key_free(key);
    key = NULL;
    test_check(axl_ssh_host_key_nv_read(&rec, NULL) == AXL_OK
               && axl_memcmp(rec.public_key, first, sizeof first) == 0,
               "load_or_create: the generated key was stored");

    /* Every later start: the same key. */
    test_check(axl_ssh_host_key_nv_load_or_create(&key, &origin, &why) == AXL_OK
               && origin == AXL_SSH_HOST_KEY_LOADED && hk_key_public_is(key, first),
               "load_or_create: a stored key -> LOADED, the same key");
    axl_pk_key_free(key);
    key = NULL;

    /* A read ERROR is not "absent": nothing is generated, nothing written. */
    size_t before_n = 0;
    void  *before   = hk_test_stored(&before_n);
    g_hk_fault = "nv-read";
    int rc = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    g_hk_fault = NULL;
    size_t after_n = 0;
    void  *after   = hk_test_stored(&after_n);
    test_check(rc == AXL_IO_ERROR && key == NULL
               && hk_why_is(why, "cannot read the variable"),
               "load_or_create: a read error is IO_ERROR, no key");
    test_check(before != NULL && after != NULL && before_n == after_n
               && axl_memcmp(before, after, before_n) == 0,
               "load_or_create: a read error leaves the stored key unchanged");
    axl_free(before);
    axl_free(after);

    /* Present but unusable: refused, never regenerated. */
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK, "load_or_create: cleared");
    hk_test_record(&rec);
    rec.public_key[0] ^= 1;
    test_check(axl_ssh_host_key_nv_write(&rec, NULL) == AXL_OK,
               "load_or_create: planted a key whose halves differ");
    before = hk_test_stored(&before_n);
    rc     = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    after  = hk_test_stored(&after_n);
    test_check(rc == AXL_ERR && key == NULL
               && hk_why_is(why, "the private key does not match its public key"),
               "load_or_create: an unusable stored key is refused by name");
    test_check(before != NULL && after != NULL && before_n == after_n
               && axl_memcmp(before, after, before_n) == 0,
               "load_or_create: an unusable stored key is NOT regenerated");
    axl_free(before);
    axl_free(after);

    /* The write fails: serve an ephemeral key, store nothing. */
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK, "load_or_create: cleared again");
    g_hk_fault = "nv-write";
    rc = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    g_hk_fault = NULL;
    test_check(rc == AXL_OK && key != NULL && origin == AXL_SSH_HOST_KEY_EPHEMERAL
               && hk_why_is(why, "cannot write the variable"),
               "load_or_create: a failed write -> EPHEMERAL, named");
    test_check(axl_ssh_host_key_nv_read(&rec, NULL) == AXL_NOT_FOUND,
               "load_or_create: a failed write stores nothing");
    axl_pk_key_free(key);
    key = NULL;

    /* The write does not read back: EPHEMERAL, and what was written is
       deleted again, so the next start does not load a key no client saw. */
    g_hk_fault = "nv-readback";
    rc = axl_ssh_host_key_nv_load_or_create(&key, &origin, &why);
    g_hk_fault = NULL;
    test_check(rc == AXL_OK && key != NULL && origin == AXL_SSH_HOST_KEY_EPHEMERAL
               && hk_why_is(why, "the variable did not read back"),
               "load_or_create: a bad read-back -> EPHEMERAL, named");
    test_check(axl_ssh_host_key_nv_read(&rec, NULL) == AXL_NOT_FOUND,
               "load_or_create: a bad read-back is deleted again");
    axl_pk_key_free(key);
    key = NULL;

    test_check(axl_ssh_host_key_nv_load_or_create(NULL, &origin, &why) == AXL_INVALID
               && axl_ssh_host_key_nv_load_or_create(&key, NULL, &why) == AXL_INVALID,
               "load_or_create: NULL arguments refused");
    axl_ssh_host_key_record_clear(&rec);
    test_check(axl_ssh_host_key_nv_delete() == AXL_OK, "load_or_create: cleaned up");
}

static void
test_ssh_server_set_host_key(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "set_host_key: loop");
    if (loop == NULL) {
        test_skip_n(11, "no loop");
        return;
    }
    AxlSshServer *s = axl_ssh_server_new(loop, 2252);
    test_check(s != NULL, "set_host_key: server created");
    if (s == NULL) {
        test_skip_n(10, "no server");
        axl_loop_free(loop);
        return;
    }
    char                fp[AXL_SSH_FINGERPRINT_SIZE];
    char                line[AXL_SSH_PUBLIC_LINE_SIZE];
    AxlSshHostKeyRecord rec;
    AxlPkKey           *key = NULL;

    hk_test_record(&rec);
    test_check(axl_ssh_host_key_load(&rec, &key, NULL) == AXL_OK,
               "set_host_key: the test key loads");
    axl_ssh_host_key_record_clear(&rec);

    test_check(axl_ssh_server_set_host_key(NULL, key) == AXL_INVALID
               && axl_ssh_server_set_host_key(s, NULL) == AXL_INVALID,
               "set_host_key: NULL arguments refused");
    AxlPkKey *pub_only = axl_pk_key_from_raw_public(AXL_PK_ED25519, HK_ED_PUBLIC,
                                                    sizeof HK_ED_PUBLIC);
    test_check(pub_only != NULL
               && axl_ssh_server_set_host_key(s, pub_only) == AXL_INVALID,
               "set_host_key: a public-only key refused (caller keeps it)");
    axl_pk_key_free(pub_only);

    test_check(axl_ssh_server_set_host_key(s, key) == AXL_OK,
               "set_host_key: adopts an Ed25519 private key");
    test_check(axl_ssh_server_host_key_fingerprint(s, fp, sizeof fp) == AXL_OK
               && axl_strcmp(fp, HK_ED_FINGERPRINT) == 0,
               "set_host_key: the server reports the key's fingerprint");
    test_check(axl_ssh_server_host_key_public_line(s, line, sizeof line) == AXL_OK
               && axl_strcmp(line, HK_ED_PUBLIC_LINE) == 0,
               "set_host_key: the server reports the key's public line");
    test_check(axl_ssh_server_host_key_fingerprint(s, fp, sizeof fp - 1)
                   == AXL_INVALID
               && axl_ssh_server_host_key_public_line(s, line, sizeof line - 1)
                   == AXL_INVALID,
               "set_host_key: the getters refuse a short buffer");

    /* A second key replaces (and frees) the first. */
    AxlPkKey *other = axl_pk_key_new(AXL_PK_ED25519);
    test_check(other != NULL && axl_ssh_server_set_host_key(s, other) == AXL_OK
               && axl_ssh_server_host_key_fingerprint(s, fp, sizeof fp) == AXL_OK
               && axl_strcmp(fp, HK_ED_FINGERPRINT) != 0,
               "set_host_key: a later call replaces the key");

    axl_ssh_server_free(s);
    AxlPkKey *late = axl_pk_key_new(AXL_PK_ED25519);
    test_check(axl_ssh_server_set_host_key(s, late) == AXL_ERR
               && axl_ssh_server_host_key_fingerprint(s, fp, sizeof fp) == AXL_ERR
               && axl_ssh_server_host_key_public_line(s, line, sizeof line) == AXL_ERR,
               "set_host_key: refused on a freed server");
    axl_pk_key_free(late);

    /* Not once listening. */
    axl_net_auto_init(AXL_NET_NIC_AUTO, 10);
    AxlSshServer *live  = axl_ssh_server_new(loop, 2253);
    char         *ak    = test_ssh_gen_authkey_line();
    bool          ready = live != NULL && ak != NULL
                          && axl_ssh_server_add_authorized_key(live, ak) == AXL_OK
                          && axl_ssh_server_listen(live) == AXL_OK;
    axl_free(ak);
    if (ready) {
        AxlPkKey *k2 = axl_pk_key_new(AXL_PK_ED25519);
        test_check(axl_ssh_server_set_host_key(live, k2) == AXL_ERR,
                   "set_host_key: refused once the server is listening");
        axl_pk_key_free(k2);
    } else {
        test_skip_n(1, "could not reach the listening state -- no working"
                       " TCP stack in this image");
    }
    axl_ssh_server_free(live);
    axl_loop_free(loop);
}

static int
test_ssh_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* See axl-test-9p.c: without this header the harness cannot bracket this
       binary, so neither the stall detector nor the per-binary leak verdict
       check covers it. */
    test_print_header("AxlSsh");

    /* The userauth tests verify real Ed25519 signatures and import keys
       through axl_pk_key_from_raw_public() without always going through
       axl_ssh_server_new(), which would otherwise enable it. */
    axl_pk_enable_ed25519();

    axl_printf("\n--- SSH wire codec ---\n");
    test_ssh_put_u32();
    test_ssh_get_u32();
    test_ssh_string_roundtrip();
    test_ssh_string_length_lies();
    test_ssh_codec_null_args();

    axl_printf("\n--- SSH version exchange ---\n");
    test_ssh_ident_parse();
    test_ssh_ident_unterminated_is_bounded();
    test_ssh_ident_preamble_is_bounded();

    axl_printf("\n--- SSH packet ---\n");
    test_ssh_packet_frame();
    test_ssh_packet_partial_and_hostile();
    test_ssh_packet_rfc_bounds();
    test_ssh_unimplemented();

    axl_printf("\n--- SSH kexinit ---\n");
    test_ssh_kexinit_build();
    test_ssh_kexinit_rejects_unsupported();
    test_ssh_kexinit_structure();
    test_ssh_kexinit_guess_flag();
    test_ssh_kexinit_strict_kex();
    test_ssh_kexinit_rekey();

    axl_printf("\n--- SSH kdf ---\n");
    test_ssh_kdf_chains();
    test_ssh_secure_zero();

    axl_printf("\n--- SSH kex exchange ---\n");
    test_ssh_mpint();
    test_ssh_blobs();
    test_ssh_exchange_hash();

    axl_printf("\n--- SSH aead ---\n");
    test_ssh_aead();
    test_ssh_rng_pool();
    test_ssh_txq();

    axl_printf("\n--- SSH userauth blobs ---\n");
    test_ssh_blob_parsers();
    test_ssh_userauth_signed_blob();
    test_ssh_userauth_verify_roundtrip();
    test_ssh_userauth_sig_check();
    test_ssh_userauth_user_changed();
    test_ssh_user_change_line();
    test_ssh_timeout_line();
    test_ssh_hook_hold_line();

    axl_printf("\n--- SSH authorized_keys ---\n");
    test_ssh_authkey_parse_accepts();
    test_ssh_authkey_parse_skips();
    test_ssh_authkey_parse_refuses();
    test_ssh_authkeys_store();
    test_ssh_authkeys_capacity();

    axl_printf("\n--- SSH server api ---\n");
    test_ssh_server_authorized_keys_api();
    test_ssh_load_authorized_keys_is_transactional();
    test_ssh_load_authorized_keys_utf16();
    test_ssh_server_listen_requires_a_key();
    test_ssh_defer_ring_bound();
    test_ssh_login_grace_api();
    test_ssh_max_password_failures_api();
    test_ssh_max_startups_api();
    test_ssh_banner_api();
    test_ssh_auth_hook_budget_api();
    test_ssh_channel_api();

    axl_printf("\n--- SSH global request ---\n");
    test_ssh_global_request();

    axl_printf("\n--- SSH channel message codec ---\n");
    test_ssh_chan_open_parse();
    test_ssh_chan_req_parse();
    test_ssh_chan_pty_parse();
    test_ssh_chan_winch_parse();
    test_ssh_chan_exec_parse();
    test_ssh_chan_u32_msg_parse();
    test_ssh_chan_adjust_parse();
    test_ssh_chan_data_parse();
    test_ssh_chan_ext_data_parse();
    test_ssh_chan_builders();

    axl_printf("\n--- SSH channel table ---\n");
    test_ssh_chan_ids();
    test_ssh_chan_budget_init();
    test_ssh_chan_alloc();
    test_ssh_chan_write();
    test_ssh_chan_writable();
    test_ssh_chan_plan_send();
    test_ssh_chan_inbound();
    test_ssh_chan_consume();
    test_ssh_chan_peer_adjust();
    test_ssh_chan_free();

    axl_printf("\n--- SSH rekey decisions and staged keys ---\n");
    test_ssh_rekey_due();
    test_ssh_rekey_out();
    test_ssh_rekey_in();
    test_ssh_rekey_counters();
    test_ssh_rekey_events();
    test_ssh_dirkey();
    test_ssh_rekey_limit_setter();
    test_ssh_idle_setters();
    test_ssh_idle_in_use();
    test_ssh_idle_scans();
    test_ssh_idle_probe_slot();

    axl_printf("\n--- SSH admission decision ---\n");
    test_ssh_admit_decide();

    axl_printf("\n--- SSH login-grace reap decision ---\n");
    test_ssh_reap_reason();
    test_ssh_linger_due();
    test_ssh_reap_tick_ms();

    axl_printf("\n--- SSH idle decisions ---\n");
    test_ssh_idle_alive_off();
    test_ssh_idle_probe_cadence();
    test_ssh_idle_heard_resets_clock_only();
    test_ssh_idle_answered_resets_count_only();
    test_ssh_idle_count_max_zero();
    test_ssh_idle_count_max_largest_never_drops();
    test_ssh_idle_same_now_is_idempotent();
    test_ssh_idle_output_paused();
    test_ssh_idle_input_gated();
    test_ssh_idle_unused();
    test_ssh_idle_unused_in_use();
    test_ssh_idle_channel_released();
    test_ssh_idle_ranking();
    test_ssh_idle_clock_failure();
    test_ssh_idle_reap_tick_derivation();

    axl_printf("\n--- SSH per-source penalties ---\n");
    test_ssh_penalty_classify();
    test_ssh_penalty_expiry();
    test_ssh_penalty_activation();
    test_ssh_penalty_ceiling();
    test_ssh_penalty_scope();
    test_ssh_penalty_table_full();
    test_ssh_penalty_table_ranking();
    test_ssh_penalty_capacity();
    test_ssh_penalty_drop_notice();
    test_ssh_penalty_parse_range();
    test_ssh_penalty_range_check();
    test_ssh_penalty_api();

    axl_printf("\n--- SSH failure timing ---\n");
    test_ssh_fail_delay_target();
    test_ssh_fail_reply_delay();
    test_ssh_listen_has_method();
    test_ssh_password_reaches_hook();
    test_ssh_auth_log_rate();

    axl_printf("\n--- SSH host key (P5) ---\n");
    test_ssh_host_key_load();
    test_ssh_host_key_record_from_key();
    test_ssh_host_key_nv_load_or_create();
    test_ssh_server_set_host_key();

    axl_printf("\n--- SSH auth api ---\n");
    test_ssh_auth_api();
    test_ssh_auth_request_next();
    test_ssh_live_server_cap();
    /* LAST, both -- see their comments. */
    test_ssh_auth_complete_after_free();
    test_ssh_server_free_twice();
    return test_print_results();
}

AXL_APP(test_ssh_main)
