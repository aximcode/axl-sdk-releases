/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-test-ssh.c
    Test application for AxlSsh — wire codec, packet framing, kex KDF.
**/

#include <axl.h>
#include "axl-test.h"                 /* test_check / test_print_results */
#include <axl/axl-string.h>           /* AxlString builder */
#include <axl/axl-str.h>              /* axl_memcmp */
#include "../../src/net/axl-ssh-internal.h"

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
    test_check(axl_ssh_kexinit_build(b) == AXL_OK, "ssh kexinit: build ok");
    const uint8_t *d = (const uint8_t *)axl_string_data(b);
    test_check(d[0] == AXL_SSH_MSG_KEXINIT, "ssh kexinit: message number 20");
    test_check(axl_string_len(b) > 1 + 16, "ssh kexinit: carries a 16-byte cookie");

    /* Our own KEXINIT must satisfy our own selector — the round trip. */
    test_check(axl_ssh_kexinit_select(d, axl_string_len(b)) == AXL_OK,
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
                                      axl_string_len(ok)) == AXL_OK,
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
                                          axl_string_len(b)) == AXL_ERR,
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
                                      axl_string_len(m)) == AXL_OK,
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
    test_check(axl_ssh_kexinit_select(d, full - 5) == AXL_ERR,
               "ssh kexinit: missing trailing fields refused");
    test_check(axl_ssh_kexinit_select(d, 20) == AXL_ERR,
               "ssh kexinit: truncated mid-list refused");
    test_check(axl_ssh_kexinit_select(d, 16) == AXL_ERR,
               "ssh kexinit: shorter than the cookie refused");

    /* Wrong message number, right shape. */
    AxlString *w = ssh_build_kexinit(ssh_kexinit_ok);
    ((char *)axl_string_data(w))[0] = 21;   /* SSH_MSG_NEWKEYS */
    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_string_data(w),
                                      axl_string_len(w)) == AXL_ERR,
               "ssh kexinit: wrong message number refused");
    axl_string_free(w);

    test_check(axl_ssh_kexinit_select(NULL, full) == AXL_ERR,
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

static int
test_ssh_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* See axl-test-9p.c: without this header the harness cannot bracket this
       binary, so neither the stall detector nor the per-binary leak verdict
       check covers it. */
    test_print_header("AxlSsh");

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

    axl_printf("\n--- SSH kexinit ---\n");
    test_ssh_kexinit_build();
    test_ssh_kexinit_rejects_unsupported();
    test_ssh_kexinit_structure();

    axl_printf("\n--- SSH kdf ---\n");
    test_ssh_kdf_chains();
    return test_print_results();
}

AXL_APP(test_ssh_main)
