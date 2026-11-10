/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

#include <axl.h>
#include "axl-test.h"
#include <axl/axl-str.h>
#include "../../tools/sshd-bind.h"

/* ---- recorder ------------------------------------------------------------
   Every emitted run is appended as `T"..."` or `C<letter>`, joined by a
   space.  Text is escaped so the expected strings are exact and readable:
   printable ASCII as itself, every other byte as \xNN.                     */

typedef struct {
    char   buf[256];
    size_t n;
} Rec;

static void
rec_put(Rec *r, const char *s)
{
    size_t l = axl_strlen(s);
    if (r->n + l + 1 < sizeof(r->buf)) {
        axl_memcpy(r->buf + r->n, s, l);
        r->n += l;
        r->buf[r->n] = '\0';
    }
}

static void
rec_emit(SshdInKind kind, const char *bytes, size_t len, void *ud)
{
    Rec *r = ud;
    if (r->n > 0) {
        rec_put(r, " ");
    }
    if (kind == SSHD_IN_CTRL) {
        char c[3] = { 'C', len == 1 ? bytes[0] : '?', '\0' };
        rec_put(r, c);
        return;
    }
    rec_put(r, "T\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];
        char t[8];
        if (b >= 0x20 && b < 0x7f && b != '"' && b != '\\') {
            t[0] = (char)b;
            t[1] = '\0';
        } else {
            axl_snprintf(t, sizeof(t), "\\x%02x", b);
        }
        rec_put(r, t);
    }
    rec_put(r, "\"");
}

static void
rec_reset(Rec *r)
{
    r->n = 0;
    r->buf[0] = '\0';
}

static void
feed(SshdInput *in, Rec *r, uint64_t now, const char *bytes, size_t len)
{
    sshd_input_feed(in, (const uint8_t *)bytes, len, now, rec_emit, r);
}

#define FEED(in, r, now, lit) feed((in), (r), (now), (lit), sizeof(lit) - 1)

static void
expect(const Rec *r, const char *want, const char *name)
{
    if (axl_strcmp(r->buf, want) == 0) {
        test_pass(name);
    } else {
        axl_printf("  got:  %s\n  want: %s\n", r->buf, want);
        test_fail(name);
    }
}

/* ---- input ---------------------------------------------------------------- */

static void
test_plain_text(void)
{
    SshdInput in = {0};
    Rec r = {0};
    FEED(&in, &r, 0, "ls -l");
    expect(&r, "T\"ls -l\"", "sshd-bind input: plain text is one run");
    rec_reset(&r);
    feed(&in, &r, 0, "", 0);
    expect(&r, "", "sshd-bind input: an empty packet emits nothing");
}

static void
test_escape_sequences(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "\x1b[");
    expect(&r, "", "sshd-bind esc: ESC [ alone is held");
    FEED(&in, &r, 10, "A");
    expect(&r, "T\"\\x1b[A\"", "sshd-bind esc: ESC [ / A across packets is one run");

    rec_reset(&r);
    FEED(&in, &r, 20, "\x1b");
    FEED(&in, &r, 21, "[1;5");
    FEED(&in, &r, 22, "C");
    expect(&r, "T\"\\x1b[1;5C\"", "sshd-bind esc: a modified key split three ways is one run");

    rec_reset(&r);
    FEED(&in, &r, 30, "ab\x1bOP" "cd");
    expect(&r, "T\"ab\\x1bOPcd\"", "sshd-bind esc: SS3 inside a run stays in the run");

    rec_reset(&r);
    FEED(&in, &r, 40, "\x1b" "x");
    expect(&r, "T\"\\x1bx\"", "sshd-bind esc: ESC then a non-introducer is one run (an Alt chord)");

    rec_reset(&r);
    FEED(&in, &r, 50, "\x1b\x1b[B");
    expect(&r, "T\"\\x1b\\x1b[B\"", "sshd-bind esc: ESC ESC [ B is Esc then Down");

    rec_reset(&r);
    FEED(&in, &r, 60, "\x1b[1;2;3;4;5;6;7;8~");
    expect(&r, "", "sshd-bind esc: an overlong (17-byte) sequence is dropped");
    FEED(&in, &r, 61, "q");
    expect(&r, "T\"q\"", "sshd-bind esc: input after an overlong sequence is intact");

    rec_reset(&r);
    FEED(&in, &r, 70, "\x1b[\x03");
    expect(&r, "T\"\\x1b[\" Cc", "sshd-bind esc: a control byte breaks a half sequence off as its keys, then is delivered");

    rec_reset(&r);
    FEED(&in, &r, 80, "\x1b[\xc3\xa9");
    expect(&r, "T\"\\x1b[\" T\"\\xc3\\xa9\"", "sshd-bind esc: a non-ASCII byte breaks a half sequence off in its own run");

    rec_reset(&r);
    FEED(&in, &r, 90, "\x1bO\x03");
    expect(&r, "T\"\\x1bO\" Cc", "sshd-bind esc: a control byte breaks a half SS3 off as its keys");

    rec_reset(&r);
    FEED(&in, &r, 100, "\x1b[x");
    expect(&r, "T\"\\x1b[x\"", "sshd-bind esc: ESC [ x is a complete sequence, not two runs");

    /* The longest forms a terminal sends for a modified key: modifyOtherKeys
       (10 bytes) and CSI-u. Each is one run, whole or split. */
    rec_reset(&r);
    FEED(&in, &r, 110, "\x1b[27;5;9~");
    expect(&r, "T\"\\x1b[27;5;9~\"", "sshd-bind esc: modifyOtherKeys ESC[27;5;9~ is one run");

    rec_reset(&r);
    FEED(&in, &r, 120, "\x1b[2");
    FEED(&in, &r, 121, "7;5;");
    FEED(&in, &r, 122, "9~");
    expect(&r, "T\"\\x1b[27;5;9~\"", "sshd-bind esc: ESC[27;5;9~ split 3+4+3 is one run");

    rec_reset(&r);
    FEED(&in, &r, 130, "\x1b[127;5u");
    expect(&r, "T\"\\x1b[127;5u\"", "sshd-bind esc: CSI-u ESC[127;5u is one run");

    rec_reset(&r);
    FEED(&in, &r, 140, "\x1b[1");
    FEED(&in, &r, 141, "27;5");
    FEED(&in, &r, 142, "u");
    expect(&r, "T\"\\x1b[127;5u\"", "sshd-bind esc: ESC[127;5u split 3+4+1 is one run");
}

static void
test_hold_is_observable(void)
{
    /* Hold versus bare-Esc cannot be seen in one packet (the text coalesces
       either way), so these split the sequence and look at WHEN a run appears. */
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "\x1b");
    FEED(&in, &r, 1, "O");
    expect(&r, "", "sshd-bind hold: ESC then O in the next packet is held");
    FEED(&in, &r, 2, "P");
    expect(&r, "T\"\\x1bOP\"", "sshd-bind hold: ESC / O / P across three packets is one run");

    rec_reset(&r);
    FEED(&in, &r, 10, "\x1b");
    FEED(&in, &r, 11, "[");
    expect(&r, "", "sshd-bind hold: ESC then [ in the next packet is held");

    rec_reset(&r);
    FEED(&in, &r, 20, "x");
    expect(&r, "T\"\\x1b[x\"", "sshd-bind hold: ESC / [ / x across packets is one run");

    rec_reset(&r);
    FEED(&in, &r, 30, "\x1b");
    FEED(&in, &r, 31, "x");
    expect(&r, "T\"\\x1b\" T\"x\"", "sshd-bind hold: ESC then x in the next packet is a bare Esc, then x");

    rec_reset(&r);
    FEED(&in, &r, 35, "\x1b");
    FEED(&in, &r, 36, "\r");
    expect(&r, "T\"\\x1b\" T\"\\x0d\"", "sshd-bind hold: ESC then CR in the next packet is a bare Esc, then Enter");

    rec_reset(&r);
    FEED(&in, &r, 40, "\x1b");
    FEED(&in, &r, 41, "\x1b");
    expect(&r, "T\"\\x1b\"", "sshd-bind hold: ESC ESC releases the first as a bare Esc and holds the second");
    FEED(&in, &r, 42, "[A");
    expect(&r, "T\"\\x1b\" T\"\\x1b[A\"", "sshd-bind hold: and the second then completes a sequence");
}

static void
test_discard(void)
{
    SshdInput in = {0};
    Rec r = {0};
    /* Sixteen bytes fill the reassembler; the seventeenth starts the skip. */
#define OVERLONG "\x1b[1;2;3;4;5;6;7;8"

    FEED(&in, &r, 0, OVERLONG);
    FEED(&in, &r, 1, "5 6");
    FEED(&in, &r, 2, "7;8");
    expect(&r, "", "sshd-bind discard: digits, space and ; are swallowed across packets");
    FEED(&in, &r, 3, "~z");
    expect(&r, "T\"z\"", "sshd-bind discard: the final byte ends the skip and is swallowed");

    rec_reset(&r);
    FEED(&in, &r, 10, OVERLONG "\x03q");
    expect(&r, "Cc T\"q\"", "sshd-bind discard: a control byte ends the skip and is delivered");

    rec_reset(&r);
    FEED(&in, &r, 20, OVERLONG "\xc3\xa9");
    expect(&r, "T\"\\xc3\\xa9\"", "sshd-bind discard: a non-ASCII byte ends the skip and starts a character");

    rec_reset(&r);
    FEED(&in, &r, 30, OVERLONG "\x7f" "q");
    expect(&r, "T\"\\x7fq\"", "sshd-bind discard: DEL ends the skip");

    rec_reset(&r);
    FEED(&in, &r, 40, OVERLONG);
    sshd_input_tick(&in, 40 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    FEED(&in, &r, 40 + SSHD_ESC_TIMEOUT_MS + 1, "q");
    expect(&r, "T\"q\"", "sshd-bind discard: silence for the timeout ends the skip (tick)");

    rec_reset(&r);
    FEED(&in, &r, 400, OVERLONG);
    FEED(&in, &r, 400 + SSHD_ESC_TIMEOUT_MS, "q");
    expect(&r, "T\"q\"", "sshd-bind discard: silence for the timeout ends the skip (next packet)");

    rec_reset(&r);
    FEED(&in, &r, 800, OVERLONG);
    FEED(&in, &r, 860, "1");
    FEED(&in, &r, 920, "2");
    FEED(&in, &r, 980, "~");
    FEED(&in, &r, 981, "z");
    expect(&r, "T\"z\"", "sshd-bind discard: each swallowed byte restarts the clock");

    rec_reset(&r);
    char many[80];
    for (size_t i = 0; i < sizeof(many); i++) {
        many[i] = '1';
    }
    FEED(&in, &r, 2000, OVERLONG);
    feed(&in, &r, 2001, many, 70);
    expect(&r, "T\"1111111\"", "sshd-bind discard: the skip is bounded at SSHD_DISCARD_MAX bytes");
#undef OVERLONG
}

static void
test_utf8_validity(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "\xe0\x80\x80");
    expect(&r, "", "sshd-bind utf8: an overlong E0 80 80 is dropped");
    FEED(&in, &r, 1, "\xc0\x80\xc1\xbf");
    expect(&r, "", "sshd-bind utf8: overlong C0 80 and C1 BF are dropped");
    FEED(&in, &r, 2, "\xed\xa0\x80");
    expect(&r, "", "sshd-bind utf8: a surrogate ED A0 80 is dropped");
    FEED(&in, &r, 3, "\xf4\x90\x80\x80");
    expect(&r, "", "sshd-bind utf8: a value past U+10FFFF is dropped");
    FEED(&in, &r, 4, "\xf0\x80\x80\x80");
    expect(&r, "", "sshd-bind utf8: an overlong F0 80 80 80 is dropped");
    FEED(&in, &r, 5, "\xf5\x80\x80\x80");
    expect(&r, "", "sshd-bind utf8: a lead above F4 is dropped");

    /* Each form's lowest and highest value, and both sides of the
       surrogate gap: U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF,
       U+10000, U+10FFFF. */
    FEED(&in, &r, 6, "\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf"
                     "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf");
    expect(&r, "T\"\\xc2\\x80\\xdf\\xbf\\xe0\\xa0\\x80\\xed\\x9f\\xbf\\xee\\x80\\x80"
               "\\xef\\xbf\\xbf\\xf0\\x90\\x80\\x80\\xf4\\x8f\\xbf\\xbf\"",
           "sshd-bind utf8: the boundary values that ARE valid pass");
}

static void
test_esc_timeout(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 1000, "\x1b");
    sshd_input_tick(&in, 1000 + SSHD_ESC_TIMEOUT_MS - 1, rec_emit, &r);
    expect(&r, "", "sshd-bind timeout: a lone ESC is held just inside the window");
    sshd_input_tick(&in, 1000 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    expect(&r, "T\"\\x1b\"", "sshd-bind timeout: a lone ESC is released at the window");
    rec_reset(&r);
    sshd_input_tick(&in, 5000, rec_emit, &r);
    expect(&r, "", "sshd-bind timeout: a released ESC is not released twice");

    FEED(&in, &r, 6000, "\x1b");
    FEED(&in, &r, 6000 + SSHD_ESC_TIMEOUT_MS - 1, "[");
    expect(&r, "", "sshd-bind timeout: ESC then [ inside the window waits for the final");
    FEED(&in, &r, 6150, "A");
    expect(&r, "T\"\\x1b[A\"", "sshd-bind timeout: the sequence completes");

    rec_reset(&r);
    FEED(&in, &r, 7000, "\x1b[");
    sshd_input_tick(&in, 7000 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    expect(&r, "T\"\\x1b[\"", "sshd-bind timeout: a stale ESC [ is released as its keys");

    rec_reset(&r);
    FEED(&in, &r, 8000, "\x1b");
    FEED(&in, &r, 8000 + SSHD_ESC_TIMEOUT_MS, "A");
    expect(&r, "T\"\\x1b\" T\"A\"", "sshd-bind timeout: a stale ESC is resolved before the next packet joins it");

    rec_reset(&r);
    FEED(&in, &r, 8500, "\x1b");
    FEED(&in, &r, 8500 + SSHD_ESC_TIMEOUT_MS, "[A");
    expect(&r, "T\"\\x1b\" T\"[A\"", "sshd-bind timeout: a stale ESC is not glued to a later [ into a sequence");

    rec_reset(&r);
    FEED(&in, &r, 9000, "\x1b");
    sshd_input_tick(&in, 100, rec_emit, &r);
    expect(&r, "T\"\\x1b\"", "sshd-bind timeout: a clock gone backwards releases");

    rec_reset(&r);
    FEED(&in, &r, 10000, "\xe2\x82");
    sshd_input_tick(&in, 10000 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    expect(&r, "", "sshd-bind timeout: a stale partial UTF-8 character is dropped");
    FEED(&in, &r, 10200, "x");
    expect(&r, "T\"x\"", "sshd-bind timeout: input after a dropped partial is intact");
}

static void
test_utf8(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "\xe2");
    expect(&r, "", "sshd-bind utf8: a lead byte alone is held");
    FEED(&in, &r, 1, "\x82\xac");
    expect(&r, "T\"\\xe2\\x82\\xac\"", "sshd-bind utf8: euro split 1+2 is one run");

    rec_reset(&r);
    FEED(&in, &r, 2, "a\xe2\x82");
    expect(&r, "T\"a\"", "sshd-bind utf8: text before a partial character is emitted at once");
    FEED(&in, &r, 3, "\xac" "b");
    expect(&r, "T\"a\" T\"\\xe2\\x82\\xacb\"", "sshd-bind utf8: the completed character joins the next run");

    rec_reset(&r);
    FEED(&in, &r, 4, "\xc3");
    FEED(&in, &r, 5, "\xa9");
    expect(&r, "T\"\\xc3\\xa9\"", "sshd-bind utf8: a 2-byte character split 1+1");

    rec_reset(&r);
    FEED(&in, &r, 6, "\xe2" "a");
    expect(&r, "T\"a\"", "sshd-bind utf8: a bad continuation drops the lead and keeps the byte");

    rec_reset(&r);
    FEED(&in, &r, 7, "\x80\xff" "z");
    expect(&r, "T\"z\"", "sshd-bind utf8: a stray continuation and an invalid byte are dropped");

    rec_reset(&r);
    FEED(&in, &r, 8, "\xf0\x9f");
    FEED(&in, &r, 9, "\x98\x80");
    expect(&r, "T\"\\xf0\\x9f\\x98\\x80\"", "sshd-bind utf8: a 4-byte character split 2+2 is one run");
}

static void
test_enter(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "a\nb");
    expect(&r, "T\"a\\x0db\"", "sshd-bind enter: LF becomes CR");

    rec_reset(&r);
    FEED(&in, &r, 1, "a\r\nb");
    expect(&r, "T\"a\\x0db\"", "sshd-bind enter: CRLF is one CR");

    rec_reset(&r);
    FEED(&in, &r, 2, "x\r");
    FEED(&in, &r, 3, "\ny");
    expect(&r, "T\"x\\x0d\" T\"y\"", "sshd-bind enter: CR then LF in the next packet is one CR");

    rec_reset(&r);
    FEED(&in, &r, 4, "\n\n");
    expect(&r, "T\"\\x0d\\x0d\"", "sshd-bind enter: two LFs are two CRs");

    rec_reset(&r);
    FEED(&in, &r, 5, "\r\r\n");
    expect(&r, "T\"\\x0d\\x0d\"", "sshd-bind enter: CR CRLF is two CRs");

    rec_reset(&r);
    FEED(&in, &r, 6, "\r");
    FEED(&in, &r, 7, "a");
    FEED(&in, &r, 8, "\n");
    expect(&r, "T\"\\x0d\" T\"a\" T\"\\x0d\"", "sshd-bind enter: a byte between CR and LF makes the LF an Enter");

    rec_reset(&r);
    FEED(&in, &r, 9, "\r");
    sshd_input_tick(&in, 9 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    FEED(&in, &r, 9 + SSHD_ESC_TIMEOUT_MS + 1, "\n");
    expect(&r, "T\"\\x0d\" T\"\\x0d\"", "sshd-bind enter: an LF long after a CR is a fresh Enter");
}

static void
test_control_keys(void)
{
    SshdInput in = {0};
    Rec r = {0};

    FEED(&in, &r, 0, "ab\x03" "cd");
    expect(&r, "T\"ab\" Cc T\"cd\"", "sshd-bind ctrl: 0x03 mid-text is TEXT, CTRL c, TEXT in order");

    rec_reset(&r);
    FEED(&in, &r, 1, "\x13");
    expect(&r, "Cs", "sshd-bind ctrl: 0x13 is CTRL s");

    rec_reset(&r);
    FEED(&in, &r, 2, "\x01\x1a");
    expect(&r, "Ca Cz", "sshd-bind ctrl: the ends of the range are a and z");

    rec_reset(&r);
    FEED(&in, &r, 3, "\x08\x09");
    expect(&r, "T\"\\x08\\x09\"", "sshd-bind ctrl: BS and TAB stay text");

    rec_reset(&r);
    FEED(&in, &r, 4, "\x1c\x1f\x7f");
    expect(&r, "T\"\\x1c\\x1f\\x7f\"", "sshd-bind ctrl: 0x1c-0x1f and DEL stay text");

    rec_reset(&r);
    FEED(&in, &r, 5, "\x00");
    expect(&r, "T\"\\x00\"", "sshd-bind ctrl: NUL (Ctrl-Space) is one TEXT run");

    rec_reset(&r);
    FEED(&in, &r, 6, "\x1b\x18");
    expect(&r, "T\"\\x1b\\x18\"", "sshd-bind ctrl: ESC 0x18 (Ctrl-Alt-X) is one TEXT run, not Esc + CTRL x");

    rec_reset(&r);
    FEED(&in, &r, 7, "\x1b\x03");
    expect(&r, "T\"\\x1b\\x03\"", "sshd-bind ctrl: ESC 0x03 in one packet is one TEXT run (Ctrl-Alt-C, no break)");

    rec_reset(&r);
    FEED(&in, &r, 7, "\x1b");
    FEED(&in, &r, 8, "\x03");
    expect(&r, "T\"\\x1b\" Cc", "sshd-bind ctrl: ESC / 0x03 across packets is Esc, then the CTRL c break");

    rec_reset(&r);
    FEED(&in, &r, 8, "\x1b\x1b");
    expect(&r, "T\"\\x1b\\x1b\"", "sshd-bind ctrl: ESC ESC in one packet is one TEXT run (Alt+Esc)");

    rec_reset(&r);
    FEED(&in, &r, 8, "\x1b");
    FEED(&in, &r, 9, "\x1b");
    sshd_input_tick(&in, 9 + SSHD_ESC_TIMEOUT_MS, rec_emit, &r);
    expect(&r, "T\"\\x1b\" T\"\\x1b\"", "sshd-bind ctrl: ESC / ESC across packets is Esc, then Esc");

    rec_reset(&r);
    FEED(&in, &r, 9, "\x1b\r\n" "a");
    expect(&r, "T\"\\x1b\\x0da\"", "sshd-bind ctrl: ESC CR is one unit, and its LF is the CRLF tail");

    rec_reset(&r);
    FEED(&in, &r, 10, "\x1b\n");
    expect(&r, "T\"\\x1b\\x0d\"", "sshd-bind ctrl: ESC LF is ESC CR (Alt-Enter)");

    rec_reset(&r);
    FEED(&in, &r, 11, "\x03");
    expect(&r, "Cc", "sshd-bind ctrl: 0x03 is still a CTRL run c");
}

static void
test_paste_lf_is_enter(void)
{
    SshdInput in = {0};
    Rec r = {0};

    /* No 0x0a reaches the decoder, which would read it as Ctrl+J. */
    FEED(&in, &r, 0, "echo a\necho b\n");
    expect(&r, "T\"echo a\\x0decho b\\x0d\"", "sshd-bind paste: LF line endings arrive as CR, no 0x0a");
}

static void
test_esc_alone_then_letter(void)
{
    SshdInput in = {0};
    Rec r = {0};

    /* Esc, then a letter typed after the escape timeout: two keys, never the
       Alt chord ESC x. */
    FEED(&in, &r, 0, "\x1b");
    sshd_input_tick(&in, 150, rec_emit, &r);
    FEED(&in, &r, 150, "x");
    expect(&r, "T\"\\x1b\" T\"x\"", "sshd-bind esc: ESC, a timeout, then x are separate runs");
}

static void
test_input_isolation(void)
{
    SshdInput a = {0};
    SshdInput b = {0};
    Rec ra = {0};
    Rec rb = {0};

    FEED(&a, &ra, 0, "\x1b[");
    FEED(&b, &rb, 0, "x");
    FEED(&a, &ra, 1, "A");
    expect(&ra, "T\"\\x1b[A\"", "sshd-bind input: two sessions reassemble independently (a)");
    expect(&rb, "T\"x\"", "sshd-bind input: two sessions reassemble independently (b)");
}

/* ---- sessions ------------------------------------------------------------- */

typedef struct {
    char   buf[64];
    size_t n;
    SshdSessions *t;
    void *remove_chan;      /* removed when `trigger` is visited */
    void *trigger;
    void *add_chan;         /* added when `trigger` is visited */
} Walk;

static void
walk_visit(SshdSession *s, void *ud)
{
    Walk *w = ud;
    w->buf[w->n++] = *(char *)s->chan;
    w->buf[w->n] = '\0';
    if (s->chan == w->trigger) {
        if (w->remove_chan != NULL) {
            sshd_sessions_remove(w->t, w->remove_chan);
        }
        if (w->add_chan != NULL) {
            sshd_sessions_add(w->t, w->add_chan);
        }
    }
}

static void
walk(SshdSessions *t, SshdSessSel sel, Walk *w)
{
    w->n = 0;
    w->buf[0] = '\0';
    w->t = t;
    sshd_sessions_each(t, sel, walk_visit, w);
}

static void
test_sessions(void)
{
    static char ca = 'a', cb = 'b', cc = 'c', cd = 'd';
    SshdSessions t = {0};
    Walk w = {0};

    test_check(sshd_sessions_add(&t, NULL) == NULL, "sshd-bind sessions: a NULL channel is refused");
    SshdSession *sa = sshd_sessions_add(&t, &ca);
    test_check(sa != NULL && sa->behind && sa->chan == &ca,
               "sshd-bind sessions: a new session is admitted BEHIND");
    test_check(sshd_sessions_add(&t, &ca) == NULL, "sshd-bind sessions: a duplicate is refused");
    sshd_sessions_add(&t, &cb);
    sshd_sessions_add(&t, &cc);
    test_check(sshd_sessions_count(&t) == 3, "sshd-bind sessions: three admitted");

    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "") == 0, "sshd-bind sessions: nothing is live until cleared");

    test_check(sshd_sessions_clear_behind(&t, &ca) && sshd_sessions_clear_behind(&t, &cb)
               && sshd_sessions_clear_behind(&t, &cc),
               "sshd-bind sessions: clear-behind finds each");
    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "abc") == 0, "sshd-bind sessions: live walk is admission order");

    test_check(sshd_sessions_mark_behind(&t, &cb), "sshd-bind sessions: mark-behind finds b");
    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "ac") == 0, "sshd-bind sessions: a BEHIND session is skipped");
    walk(&t, SSHD_SESS_BEHIND, &w);
    test_check(axl_strcmp(w.buf, "b") == 0, "sshd-bind sessions: the behind walk finds only it");
    walk(&t, SSHD_SESS_ANY, &w);
    test_check(axl_strcmp(w.buf, "abc") == 0, "sshd-bind sessions: the any walk finds all");

    sshd_sessions_clear_behind(&t, &cb);
    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "abc") == 0, "sshd-bind sessions: clearing re-includes");

    test_check(!sshd_sessions_mark_behind(&t, &cd) && !sshd_sessions_clear_behind(&t, &cd)
               && !sshd_sessions_remove(&t, &cd) && sshd_sessions_find(&t, &cd) == NULL,
               "sshd-bind sessions: an unknown channel is reported absent");

    /* A session removes itself mid-walk. */
    w.trigger = &cb; w.remove_chan = &cb; w.add_chan = NULL;
    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "abc") == 0 && sshd_sessions_count(&t) == 2
               && sshd_sessions_find(&t, &cb) == NULL,
               "sshd-bind sessions: removing the visited session mid-walk is safe");

    /* A session removes a LATER one: it is not visited. */
    sshd_sessions_add(&t, &cb);
    sshd_sessions_clear_behind(&t, &cb);       /* order is now a c b */
    w.trigger = &ca; w.remove_chan = &cc; w.add_chan = NULL;
    walk(&t, SSHD_SESS_LIVE, &w);
    test_check(axl_strcmp(w.buf, "ab") == 0,
               "sshd-bind sessions: a session removed before its turn is not visited");

    /* An add during the walk is not visited, even in a freed earlier slot. */
    w.trigger = &cb; w.remove_chan = NULL; w.add_chan = &cd;
    sshd_sessions_clear_behind(&t, &ca);
    walk(&t, SSHD_SESS_ANY, &w);
    test_check(axl_strcmp(w.buf, "ab") == 0 && sshd_sessions_find(&t, &cd) != NULL,
               "sshd-bind sessions: a session added mid-walk is not visited by it");

    /* The table is bounded, and a freed slot is reusable. */
    SshdSessions full = {0};
    static char cs[SSHD_SESSIONS_MAX + 1];
    bool all_ok = true;
    for (int i = 0; i < SSHD_SESSIONS_MAX; i++) {
        all_ok = all_ok && sshd_sessions_add(&full, &cs[i]) != NULL;
    }
    test_check(all_ok && sshd_sessions_add(&full, &cs[SSHD_SESSIONS_MAX]) == NULL,
               "sshd-bind sessions: the table holds exactly SSHD_SESSIONS_MAX");
    sshd_sessions_remove(&full, &cs[3]);
    test_check(sshd_sessions_add(&full, &cs[SSHD_SESSIONS_MAX]) != NULL,
               "sshd-bind sessions: a removed slot is reused");
}

static void
test_session_input_state(void)
{
    static char ca = 'a';
    SshdSessions t = {0};
    Rec r = {0};
    SshdSession *s = sshd_sessions_add(&t, &ca);
    if (s == NULL) {
        test_fail("sshd-bind sessions: admit for the input-state check");
        return;
    }
    FEED(&s->in, &r, 0, "\x1b");
    sshd_sessions_remove(&t, &ca);
    s = sshd_sessions_add(&t, &ca);
    if (s == NULL) {
        test_fail("sshd-bind sessions: re-admit for the input-state check");
        return;
    }
    FEED(&s->in, &r, 1, "[");
    expect(&r, "T\"[\"", "sshd-bind sessions: a re-admitted session starts with clean input state");
}

/* A fake channel: accepts whole pieces while they fit its budget, then
   refuses (BUSY), or refuses everything for good once failed. */
typedef struct {
    char   got[128];
    size_t n;
    size_t budget;
    bool   failed;
    int    calls;
} FakeChan;

static SshdWrResult
fake_write(const char *bytes, size_t len, void *ud)
{
    FakeChan *f = (FakeChan *)ud;

    f->calls++;
    if (f->failed) {
        return SSHD_WR_FAILED;
    }
    if (len > f->budget || f->n + len >= sizeof(f->got)) {
        return SSHD_WR_BUSY;
    }
    axl_memcpy(f->got + f->n, bytes, len);
    f->n += len;
    f->got[f->n] = '\0';
    f->budget -= len;
    return SSHD_WR_OK;
}

static void
test_repaint(void)
{
    SshdRepaint r  = {0};
    FakeChan    f  = { .budget = 100 };
    const char *s1 = "SCREEN-ONE";   /* 10 bytes */

    /* Written whole in pieces of 4, nothing changed: rejoin. */
    sshd_repaint_begin(&r, 10, 7);
    test_check(r.active && r.len == 10 && r.off == 0 && r.gen == 7,
               "sshd-bind repaint: begin records length and generation");
    test_check(sshd_repaint_drain(&r, s1, 7, 4, fake_write, &f) == SSHD_RP_REJOIN,
               "sshd-bind repaint: all out and unchanged rejoins");
    test_check(axl_strcmp(f.got, "SCREEN-ONE") == 0 && f.calls == 3 && !r.active,
               "sshd-bind repaint: written in pieces of at most 4, then inactive");

    /* Refused part-way: blocked, and the offset is kept. */
    f = (FakeChan){ .budget = 4 };
    sshd_repaint_begin(&r, 10, 7);
    test_check(sshd_repaint_drain(&r, s1, 7, 4, fake_write, &f) == SSHD_RP_BLOCKED,
               "sshd-bind repaint: a refusal part-way blocks");
    test_check(axl_strcmp(f.got, "SCRE") == 0 && r.off == 4 && r.active,
               "sshd-bind repaint: the accepted piece counts and the repaint stays active");

    /* Resumed with the console unchanged: the REST, not a restart, and it
       rejoins. */
    f.budget = 100;
    test_check(sshd_repaint_drain(&r, s1, 7, 4, fake_write, &f) == SSHD_RP_REJOIN,
               "sshd-bind repaint: resumed and unchanged rejoins");
    test_check(axl_strcmp(f.got, "SCREEN-ONE") == 0,
               "sshd-bind repaint: a resume continues from the offset, nothing resent");

    /* THE STALENESS CHECK. Output kept changing while the session was
       stalled part-way through its repaint: once the old repaint is out it
       describes a screen that no longer exists, so the session must take a
       fresh one -- and that one, finished with nothing changed, rejoins. */
    f = (FakeChan){ .budget = 4 };
    sshd_repaint_begin(&r, 10, 7);
    test_check(sshd_repaint_drain(&r, s1, 7, 4, fake_write, &f) == SSHD_RP_BLOCKED,
               "sshd-bind repaint stale: blocked part-way at generation 7");
    f.budget = 100;
    test_check(sshd_repaint_drain(&r, s1, 9, 4, fake_write, &f) == SSHD_RP_RETAKE,
               "sshd-bind repaint stale: the console moved on (7 -> 9), so it is retaken");
    test_check(!r.active && axl_strcmp(f.got, "SCREEN-ONE") == 0,
               "sshd-bind repaint stale: the old one finished, and is no longer active");
    sshd_repaint_begin(&r, 10, 9);
    test_check(sshd_repaint_drain(&r, "SCREEN-TWO", 9, 4, fake_write, &f) == SSHD_RP_REJOIN
               && axl_strcmp(f.got, "SCREEN-ONESCREEN-TWO") == 0,
               "sshd-bind repaint stale: the fresh repaint is the CURRENT screen, and rejoins");

    /* A change while the whole repaint was written in ONE call is stale too
       (the generation is only compared, never assumed). */
    f = (FakeChan){ .budget = 100 };
    sshd_repaint_begin(&r, 10, 3);
    test_check(sshd_repaint_drain(&r, s1, 4, 4, fake_write, &f) == SSHD_RP_RETAKE,
               "sshd-bind repaint: any generation change is stale, even within one call");

    /* Refused for good. */
    f = (FakeChan){ .budget = 100, .failed = true };
    sshd_repaint_begin(&r, 10, 1);
    test_check(sshd_repaint_drain(&r, s1, 1, 4, fake_write, &f) == SSHD_RP_FAILED && !r.active,
               "sshd-bind repaint: a refusal for good fails and deactivates");
    test_check(sshd_repaint_drain(&r, s1, 1, 4, fake_write, &f) == SSHD_RP_FAILED,
               "sshd-bind repaint: draining an inactive repaint fails");
}

static void
test_deferral(void)
{
    test_check(sshd_bind_snapshot_allowed(0), "sshd-bind defer: a snapshot is allowed with no op in flight");
    test_check(!sshd_bind_snapshot_allowed(1), "sshd-bind defer: refused with one op in flight");
    test_check(!sshd_bind_snapshot_allowed(3), "sshd-bind defer: refused with nested ops in flight");
}

/* ---- fan-out ------------------------------------------------------------- */

/* A channel as AxlSsh queues it: each write is all or nothing against a
   queue limit, and the client reads (drains) the queue at its own pace. What
   the client has received, in order, is the stream it renders. */
typedef struct {
    char          got[256];   ///< every byte ever accepted, in order
    size_t        n;
    size_t        queued;     ///< accepted but not yet read by the client
    size_t        cap;        ///< the channel's queue limit
    int           calls;
    SshdWrResult  force;      ///< answer every write with this, unless OK
} FanChan;

static SshdWrResult
fan_chan_write(const char *bytes, size_t len, void *ud)
{
    FanChan *c = (FanChan *)ud;

    c->calls++;
    if (c->force != SSHD_WR_OK) {
        return c->force;
    }
    if (c->queued + len > c->cap || c->n + len >= sizeof(c->got)) {
        return SSHD_WR_BUSY;
    }
    axl_memcpy(c->got + c->n, bytes, len);
    c->n += len;
    c->got[c->n] = '\0';
    c->queued += len;
    return SSHD_WR_OK;
}

/* The Shell has exited and the console is being given back: a busy device is
   retried, and only a device that really went is reported as released. */
static void
test_release(void)
{
    test_check(sshd_bind_release_of(AXL_BUSY) == SSHD_REL_RETRY,
               "sshd-bind release: a call in flight is retried");
    test_check(sshd_bind_release_of(AXL_OK) == SSHD_REL_RELEASED,
               "sshd-bind release: AXL_OK is released");
    test_check(sshd_bind_release_of(AXL_ERR) == SSHD_REL_STUCK
                   && sshd_bind_release_of(AXL_INVALID) == SSHD_REL_STUCK,
               "sshd-bind release: a refusal for good is stuck, never released");
    const char *g = sshd_bind_goodbye(SSHD_REL_RELEASED);
    test_check(g != NULL
                   && axl_strcmp(g, "[sshd] the UEFI Shell exited; console released") == 0,
               "sshd-bind release: the released goodbye");
    g = sshd_bind_goodbye(SSHD_REL_STUCK);
    test_check(g != NULL
                   && axl_strcmp(g, "[sshd] the UEFI Shell exited; the console could "
                                    "not be released") == 0,
               "sshd-bind release: the stuck goodbye says the console is still taken");
    test_check(sshd_bind_goodbye(SSHD_REL_RETRY) == NULL,
               "sshd-bind release: nothing is said while a release is retried");
    test_check(axl_strcmp(SSHD_STOPPED_TEXT, "[sshd] stopped") == 0,
               "sshd-bind release: the stop goodbye");
}

/* The goodbye's bytes: a scroll region set for a larger terminal is reset
   with the cursor saved and restored around it (DECSTBM homes the cursor, so
   the line would land on row 2 over the console's text), and the line
   erases to its end so no old text trails it. */
static void
test_goodbye_text(void)
{
    char   buf[160];
    size_t n;

    n = sshd_bind_goodbye_text(buf, sizeof(buf), true, SSHD_STOPPED_TEXT);
    test_check(axl_strcmp(buf, "\x1b" "7" "\x1b[r" "\x1b" "8"
                               "\r\n[sshd] stopped\x1b[K\r\n") == 0
                   && n == axl_strlen(buf),
               "sshd-bind goodbye: a region is reset with the cursor kept, then the line");
    n = sshd_bind_goodbye_text(buf, sizeof(buf), false, SSHD_STOPPED_TEXT);
    test_check(axl_strcmp(buf, "\r\n[sshd] stopped\x1b[K\r\n") == 0 && n == 21,
               "sshd-bind goodbye: no region, the line alone, erased to its end");
    n = sshd_bind_goodbye_text(buf, sizeof(buf), true, sshd_bind_goodbye(SSHD_REL_RELEASED));
    test_check(axl_strcmp(buf, "\x1b" "7" "\x1b[r" "\x1b" "8"
                               "\r\n[sshd] the UEFI Shell exited; console released"
                               "\x1b[K\r\n") == 0,
               "sshd-bind goodbye: the Shell-exit line, region reset");
    /* "\r\n[sshd] stopped\x1b[K\r\n" is 21 bytes: 22 fits it with its NUL. */
    n = sshd_bind_goodbye_text(buf, 22, false, SSHD_STOPPED_TEXT);
    test_check(n == 21 && axl_strcmp(buf, "\r\n[sshd] stopped\x1b[K\r\n") == 0,
               "sshd-bind goodbye: an exact fit is written");
    n = sshd_bind_goodbye_text(buf, 21, false, SSHD_STOPPED_TEXT);
    test_check(n == 0 && buf[0] == '\0',
               "sshd-bind goodbye: one byte short writes nothing, never a cut sequence");
}

static void
test_fan_write(void)
{
    /* Three flushes, each a run of whole escape sequences and characters. */
    static const char f1[] = "\x1b[1;31mAB\x1b[0m";          /* 15 bytes */
    static const char f2[] = "\x1b[2;5HCD\x1b[7mEF\x1b[0m";  /* 20 bytes */
    static const char f3[] = "\x1b[3;1HGH";                  /* 9 bytes  */
    FanChan c = { .cap = 24 };

    test_check(sshd_fan_write(f1, sizeof(f1) - 1, 24, fan_chan_write, &c) == SSHD_FAN_SENT
               && c.calls == 1,
               "sshd-bind fan: a flush that fits is sent in ONE write");
    /* The client has read nothing: 15 queued, the next flush needs 20 of 24. */
    test_check(sshd_fan_write(f2, sizeof(f2) - 1, 24, fan_chan_write, &c) == SSHD_FAN_BEHIND,
               "sshd-bind fan: a flush the queue cannot take is BEHIND");
    test_check(axl_strcmp(c.got, f1) == 0,
               "sshd-bind fan: the refused flush queued no part of itself");
    c.queued = 0;   /* the client catches up */
    test_check(sshd_fan_write(f3, sizeof(f3) - 1, 24, fan_chan_write, &c) == SSHD_FAN_SENT,
               "sshd-bind fan: after the client drains, the next flush is sent");
    {
        char want[64];
        axl_snprintf(want, sizeof(want), "%s%s", f1, f3);
        test_check(axl_strcmp(c.got, want) == 0,
                   "sshd-bind fan: the stream holds only whole flushes (no torn sequence)");
    }

    /* Longer than the limit: never attempted. */
    c = (FanChan){ .cap = 24 };
    test_check(sshd_fan_write(f2, sizeof(f2) - 1, 16, fan_chan_write, &c) == SSHD_FAN_TOO_BIG
               && c.calls == 0 && c.n == 0,
               "sshd-bind fan: a flush over the limit is TOO_BIG and writes nothing");
    test_check(sshd_fan_write(f1, sizeof(f1) - 1, sizeof(f1) - 1, fan_chan_write, &c)
               == SSHD_FAN_SENT,
               "sshd-bind fan: a flush exactly at the limit is sent");
    c = (FanChan){ .cap = 24 };
    test_check(sshd_fan_write(NULL, 0, 24, fan_chan_write, &c) == SSHD_FAN_SENT
               && c.calls == 0,
               "sshd-bind fan: an empty flush writes nothing");

    c = (FanChan){ .cap = 24, .force = SSHD_WR_FAILED };
    test_check(sshd_fan_write(f1, sizeof(f1) - 1, 24, fan_chan_write, &c) == SSHD_FAN_FAILED,
               "sshd-bind fan: a refusal for good is FAILED");
    c = (FanChan){ .cap = 24, .force = SSHD_WR_GONE };
    test_check(sshd_fan_write(f1, sizeof(f1) - 1, 24, fan_chan_write, &c) == SSHD_FAN_GONE,
               "sshd-bind fan: an already-closed channel is GONE");

    test_check(sshd_bind_close_on(SSHD_WR_FAILED),
               "sshd-bind close-on: a refusal for good closes the channel");
    test_check(!sshd_bind_close_on(SSHD_WR_BUSY) && !sshd_bind_close_on(SSHD_WR_GONE)
               && !sshd_bind_close_on(SSHD_WR_OK),
               "sshd-bind close-on: BUSY, GONE and OK do not");
}

/* ---- pending input --------------------------------------------------------- */

/* A taker that takes at most `budget` bytes in all, recording what it took. */
typedef struct {
    Rec    rec;
    size_t budget;
    bool   ctrl_refuse;
} Taker;

static size_t
taker_take(SshdInKind kind, const char *bytes, size_t len, void *ud)
{
    Taker *t = (Taker *)ud;

    if (kind == SSHD_IN_CTRL && t->ctrl_refuse) {
        return 0;
    }
    size_t n = len < t->budget ? len : t->budget;
    if (n > 0) {
        rec_emit(kind, bytes, n, &t->rec);
    }
    t->budget -= n;
    return n;
}

static void
test_pend(void)
{
    static char big[40000];
    Taker    t    = { .budget = 1000 };
    SshdPend pend = {0};

    /* A session that never pastes costs a few words, not the queue: the
       buffer arrives with the first run that has to wait. */
    test_check(sizeof(SshdPend) <= 64,
               "sshd-bind pend: an unused queue is small (the buffer is not embedded)");
    test_check(sshd_pend_empty(&pend), "sshd-bind pend: starts empty");
    test_check(sshd_pend_drain(&pend, taker_take, &t) && pend.buf == NULL,
               "sshd-bind pend: draining an empty queue allocates nothing");
    axl_mem_fail_next_alloc(1);
    test_check(!sshd_pend_push(&pend, SSHD_IN_TEXT, "abc", 3) && pend.dropped == 3
                   && sshd_pend_empty(&pend),
               "sshd-bind pend: a push that cannot allocate the queue is refused and "
               "counted");
    pend.dropped = 0;
    test_check(sshd_pend_push(&pend, SSHD_IN_TEXT, "abc", 3)
               && sshd_pend_push(&pend, SSHD_IN_CTRL, "c", 1)
               && sshd_pend_push(&pend, SSHD_IN_TEXT, "\x1b[", 2)
               && sshd_pend_push(&pend, SSHD_IN_TEXT, "A", 1),
               "sshd-bind pend: runs queue");
    test_check(!sshd_pend_empty(&pend), "sshd-bind pend: not empty once queued");
    test_check(sshd_pend_drain(&pend, taker_take, &t) && sshd_pend_empty(&pend),
               "sshd-bind pend: a drain with room empties it");
    expect(&t.rec, "T\"abc\" Cc T\"\\x1b[\" T\"A\"",
           "sshd-bind pend: runs come back in order, each on its own -- a timed-out "
           "prefix is never glued to the next run");

    /* Taken part-way: the rest stays, and comes back next time. */
    t = (Taker){ .budget = 2 };
    sshd_pend_push(&pend, SSHD_IN_TEXT, "abcd", 4);
    sshd_pend_push(&pend, SSHD_IN_TEXT, "ef", 2);
    test_check(!sshd_pend_drain(&pend, taker_take, &t),
               "sshd-bind pend: a drain the taker stops is not empty");
    t.budget = 1000;
    test_check(sshd_pend_drain(&pend, taker_take, &t),
               "sshd-bind pend: the next drain finishes it");
    expect(&t.rec, "T\"ab\" T\"cd\" T\"ef\"",
           "sshd-bind pend: a run taken part-way resumes where it stopped");

    /* A CTRL key is all or nothing, and order holds behind it. */
    t = (Taker){ .budget = 1000, .ctrl_refuse = true };
    sshd_pend_push(&pend, SSHD_IN_CTRL, "c", 1);
    sshd_pend_push(&pend, SSHD_IN_TEXT, "x", 1);
    test_check(!sshd_pend_drain(&pend, taker_take, &t) && t.rec.n == 0,
               "sshd-bind pend: a refused CTRL key holds everything behind it");
    t.ctrl_refuse = false;
    test_check(sshd_pend_drain(&pend, taker_take, &t), "sshd-bind pend: then it goes");
    expect(&t.rec, "Cc T\"x\"", "sshd-bind pend: the CTRL key, then the text");

    /* Bounded, all or nothing per run. */
    axl_memset(big, 'p', sizeof(big));
    test_check(sshd_pend_push(&pend, SSHD_IN_TEXT, big, sizeof(big)),
               "sshd-bind pend: a 40000-byte paste is held");
    test_check(!sshd_pend_push(&pend, SSHD_IN_TEXT, big, 30000)
               && pend.dropped == 30000,
               "sshd-bind pend: a run that does not fit whole is refused and counted");
    test_check(!sshd_pend_push(&pend, SSHD_IN_TEXT, big, 0)
               && !sshd_pend_push(&pend, SSHD_IN_TEXT, big, 65536),
               "sshd-bind pend: an empty run or one over 65535 bytes is refused");
    /* 30000 taken from the front: 10000 left, so 30000 more now fits only if
       the freed front is reused. */
    t = (Taker){ .budget = 30000 };
    sshd_pend_drain(&pend, taker_take, &t);
    test_check(sshd_pend_push(&pend, SSHD_IN_TEXT, "TAIL", 4)
               && sshd_pend_push(&pend, SSHD_IN_TEXT, big, 30000)
               && sshd_pend_push(&pend, SSHD_IN_TEXT, big, 25000),
               "sshd-bind pend: space freed at the front is reused");
    t = (Taker){ .budget = 100000 };
    test_check(sshd_pend_drain(&pend, taker_take, &t) && t.budget == 100000 - 65004,
               "sshd-bind pend: every queued byte comes back exactly once");

    test_check(pend.buf != NULL, "sshd-bind pend: the buffer was allocated by a push");
    sshd_pend_free(&pend);
    test_check(pend.buf == NULL && sshd_pend_empty(&pend),
               "sshd-bind pend: free releases it and leaves an empty queue");
    sshd_pend_free(&pend);   /* twice is harmless; the leak gate sees the rest */
}

/* A console ring the guest reads by hand: text adds unread keys, an ordered
   control key goes in only once the guest has read them (sshd_ctrl_may_enter). */
typedef struct {
    Rec    rec;
    size_t unread;
} Ring;

static size_t
ring_take(SshdInKind kind, const char *bytes, size_t len, void *ud)
{
    Ring *r = (Ring *)ud;

    if (kind == SSHD_IN_CTRL && !sshd_ctrl_may_enter(bytes[0], r->unread)) {
        return 0;
    }
    rec_emit(kind, bytes, len, &r->rec);
    r->unread += kind == SSHD_IN_CTRL ? 1 : len;
    return len;
}

/* 'echo abcdef' + ^S in one write: the ^S waits for the guest to read the
   text, so its notify (the Shell's halt) acts after it, not inside it. */
static void
test_ctrl_waits_for_reads(void)
{
    Ring     ring = {0};
    SshdPend pend = {0};

    for (int l = 'a'; l <= 'z'; l++) {
        bool ok = sshd_ctrl_may_enter((char)l, 0)
                  && sshd_ctrl_may_enter((char)l, 5) == (l == 'c');
        if (!ok) {
            axl_printf("  Ctrl-%c\n", l);
            test_fail("sshd-bind ctrl: every key may enter an empty ring; with unread "
                      "keys, only Ctrl-C");
            break;
        }
        if (l == 'z') {
            test_pass("sshd-bind ctrl: every key may enter an empty ring; with unread "
                      "keys, only Ctrl-C");
        }
    }
    test_check(sshd_input_route(&pend, SSHD_IN_TEXT, "echo abcdef", 11, ring_take, &ring)
                   && sshd_input_route(&pend, SSHD_IN_CTRL, "s", 1, ring_take, &ring),
               "sshd-bind ctrl: text then ^S route without a drop");
    expect(&ring.rec, "T\"echo abcdef\"",
           "sshd-bind ctrl: the ^S is held while the text is unread");
    test_check(!sshd_pend_empty(&pend), "sshd-bind ctrl: it waits in the queue");
    ring.unread = 3;   /* the guest has read some, not all */
    sshd_pend_drain(&pend, ring_take, &ring);
    expect(&ring.rec, "T\"echo abcdef\"",
           "sshd-bind ctrl: still held while any key is unread");
    test_check(sshd_input_route(&pend, SSHD_IN_TEXT, "x", 1, ring_take, &ring),
               "sshd-bind ctrl: text typed after the ^S queues behind it");
    test_check(sshd_input_route(&pend, SSHD_IN_CTRL, "c", 1, ring_take, &ring),
               "sshd-bind ctrl: a Ctrl-C goes ahead");
    expect(&ring.rec, "T\"echo abcdef\" Cc",
           "sshd-bind ctrl: Ctrl-C overtakes the held ^S at once");
    ring.unread = 0;   /* the guest has read everything */
    sshd_pend_drain(&pend, ring_take, &ring);
    expect(&ring.rec, "T\"echo abcdef\" Cc Cs T\"x\"",
           "sshd-bind ctrl: then the ^S goes in, and the text typed after it");
    test_check(sshd_pend_empty(&pend), "sshd-bind ctrl: nothing is left waiting");
    sshd_pend_free(&pend);
}

/* Only Ctrl-C overtakes: a paste into a reader followed by ^Q stays in
   order, so an editor's ^Q acts after the text, not part-way through it. */
static void
test_route(void)
{
    Taker    t    = { .budget = 5 };
    SshdPend pend = {0};

    for (int l = 'a'; l <= 'z'; l++) {
        if (sshd_ctrl_bypasses((char)l) != (l == 'c')) {
            axl_printf("  Ctrl-%c\n", l);
            test_fail("sshd-bind route: only Ctrl-C bypasses the queue");
            break;
        }
        if (l == 'z') {
            test_pass("sshd-bind route: only Ctrl-C bypasses the queue");
        }
    }

    /* The ring takes five keys; the rest of the paste waits. */
    test_check(sshd_input_route(&pend, SSHD_IN_TEXT, "abcdefgh", 8, taker_take, &t)
                   && !sshd_pend_empty(&pend),
               "sshd-bind route: a paste the console cannot take whole waits");
    t.budget = 100;
    test_check(sshd_input_route(&pend, SSHD_IN_CTRL, "q", 1, taker_take, &t),
               "sshd-bind route: a ^Q behind it is queued");
    expect(&t.rec, "T\"abcde\"",
           "sshd-bind route: a ^Q is not offered while a paste waits, room or not");
    t.ctrl_refuse = true;
    test_check(sshd_input_route(&pend, SSHD_IN_CTRL, "c", 1, taker_take, &t),
               "sshd-bind route: a refused Ctrl-C is not an error");
    t.ctrl_refuse = false;
    test_check(sshd_input_route(&pend, SSHD_IN_CTRL, "c", 1, taker_take, &t),
               "sshd-bind route: a Ctrl-C is routed");
    expect(&t.rec, "T\"abcde\" Cc",
           "sshd-bind route: Ctrl-C goes at once, ahead of the paste, and a refused one "
           "is never queued to be offered again");
    test_check(sshd_pend_drain(&pend, taker_take, &t),
               "sshd-bind route: the queue drains");
    expect(&t.rec, "T\"abcde\" Cc T\"fgh\" Cq",
           "sshd-bind route: the paste's rest, then the ^Q -- in the order typed");

    /* With nothing pending an ordered control key goes at once; refused, it
       waits and keeps the text behind it waiting. */
    t = (Taker){ .budget = 100, .ctrl_refuse = true };
    test_check(sshd_input_route(&pend, SSHD_IN_CTRL, "s", 1, taker_take, &t)
                   && sshd_input_route(&pend, SSHD_IN_TEXT, "x", 1, taker_take, &t)
                   && t.rec.n == 0 && !sshd_pend_empty(&pend),
               "sshd-bind route: a refused ^S waits, and the text typed after it too");
    t.ctrl_refuse = false;
    test_check(sshd_pend_drain(&pend, taker_take, &t), "sshd-bind route: then both go");
    expect(&t.rec, "Cs T\"x\"", "sshd-bind route: the ^S, then the text");
    sshd_pend_free(&pend);
}

/* A setup that fails after the take-over was installed: the image may be
   unloaded (setup returns an error) only when the console is given back. A
   busy or stuck device still calls into this image, so it must stay resident
   with `sshd stop` able to retry -- the same hold the Shell-exit path keeps. */
static void
test_setup_failed(void)
{
    static const int rcs[] = { AXL_OK, AXL_BUSY, AXL_ERR, -3, -99 };

    test_check(sshd_bind_setup_failed(SSHD_REL_RELEASED) == SSHD_SETUP_FAIL,
               "sshd-bind setup-fail: a released console lets setup fail and the image unload");
    test_check(sshd_bind_setup_failed(SSHD_REL_STUCK) == SSHD_SETUP_STAY,
               "sshd-bind setup-fail: a stuck take-over keeps the image resident");
    test_check(sshd_bind_setup_failed(SSHD_REL_RETRY) == SSHD_SETUP_STAY,
               "sshd-bind setup-fail: a busy device keeps the image resident too");

    /* The invariant, over every way an uninstall can answer: the image is
       allowed to unload only when the take-over state is NONE. */
    for (size_t i = 0; i < sizeof(rcs) / sizeof(rcs[0]); i++) {
        SshdCon c = SSHD_CON_TAKEN;
        SshdRelease r = sshd_bind_con_drop(&c, rcs[i]);

        test_check((sshd_bind_setup_failed(r) == SSHD_SETUP_FAIL) == (c == SSHD_CON_NONE),
                   "sshd-bind setup-fail: the image unloads exactly when the console is back");
    }
}

/* The take-over's release, across a stuck uninstall: the handle is kept, the
   unload refused, the status says so, and a later attempt can still succeed. */
static void
test_con_drop(void)
{
    SshdCon c = SSHD_CON_TAKEN;

    test_check(sshd_bind_con_drop(&c, AXL_BUSY) == SSHD_REL_RETRY && c == SSHD_CON_TAKEN,
               "sshd-bind con: a busy device changes nothing");
    test_check(sshd_bind_con_drop(&c, AXL_ERR) == SSHD_REL_STUCK && c == SSHD_CON_STUCK,
               "sshd-bind con: a failed uninstall is stuck and keeps the handle");
    test_check(axl_strcmp(sshd_bind_status(true, true, true),
                          "stuck - the console take-over could not be undone; "
                          "'sshd stop' tries again") == 0,
               "sshd-bind con: status reports a stuck release, not \"running\"");
    /* `sshd stop`: the next attempt runs on the kept handle, and fails. */
    test_check(sshd_bind_con_drop(&c, AXL_ERR) == SSHD_REL_STUCK && c == SSHD_CON_STUCK,
               "sshd-bind con: a stop whose uninstall fails again is refused, still stuck");
    test_check(sshd_bind_con_drop(&c, AXL_BUSY) == SSHD_REL_RETRY && c == SSHD_CON_STUCK,
               "sshd-bind con: a busy retry leaves it stuck (not taken)");
    test_check(sshd_bind_con_drop(&c, AXL_OK) == SSHD_REL_RELEASED && c == SSHD_CON_NONE,
               "sshd-bind con: a retry that succeeds gives the console back");

    test_check(axl_strcmp(sshd_bind_status(false, false, false), "stopped") == 0
                   && axl_strcmp(sshd_bind_status(true, true, false), "running") == 0
                   && axl_strcmp(sshd_bind_status(true, false, false),
                                 "stopped (the Shell it served exited)") == 0,
               "sshd-bind con: status for stopped, running and an exited Shell");
    test_check(axl_strcmp(sshd_bind_status(true, false, true),
                          sshd_bind_status(true, true, true)) == 0,
               "sshd-bind con: stuck wins over every other reading");
}

static void
test_ratelimit(void)
{
    SshdRateLimit r = {0};
    uint32_t      n = 99;

    test_check(sshd_ratelimit(&r, 5000, 1000, &n) && n == 0,
               "sshd-bind ratelimit: the first event logs");
    test_check(!sshd_ratelimit(&r, 5100, 1000, &n) && !sshd_ratelimit(&r, 5999, 1000, &n),
               "sshd-bind ratelimit: events within the period are held back");
    test_check(sshd_ratelimit(&r, 6000, 1000, &n) && n == 2,
               "sshd-bind ratelimit: the next line carries the count held back");
    test_check(!sshd_ratelimit(&r, 6001, 1000, NULL),
               "sshd-bind ratelimit: and the period restarts from it");
    test_check(sshd_ratelimit(&r, 7001, 1000, &n) && n == 1,
               "sshd-bind ratelimit: a NULL count still counts");
}

/* A terminal of another size than the console. The console is never
   reshaped (other sessions and the local screen share it): a larger terminal
   is confined to the console's rows by a scroll region, a smaller one is held
   behind a notice until it grows. No pty, or a pty that gives no size (ssh -tt
   with a pipe for stdin sends 0x0), is taken as the console's size. */
static void
test_fit(void)
{
    test_check(sshd_fit(100, 31, 0, 0) == SSHD_FIT_EXACT, "sshd-bind fit: no size -> exact");
    test_check(sshd_fit(100, 31, 100, 31) == SSHD_FIT_EXACT, "sshd-bind fit: same size");
    test_check(sshd_fit(100, 31, 120, 40) == SSHD_FIT_LARGER, "sshd-bind fit: larger");
    test_check(sshd_fit(100, 31, 100, 40) == SSHD_FIT_LARGER, "sshd-bind fit: taller only");
    test_check(sshd_fit(100, 31, 120, 31) == SSHD_FIT_LARGER, "sshd-bind fit: wider only");
    test_check(sshd_fit(100, 31, 80, 24) == SSHD_FIT_SMALLER, "sshd-bind fit: smaller");
    test_check(sshd_fit(100, 31, 120, 24) == SSHD_FIT_SMALLER,
               "sshd-bind fit: wider but shorter is smaller");
    test_check(sshd_fit(100, 31, 99, 40) == SSHD_FIT_SMALLER,
               "sshd-bind fit: one column short is smaller");

    char   buf[256];
    size_t n = sshd_fit_prefix(buf, sizeof(buf), SSHD_FIT_LARGER, 31, false);
    test_check(n == 7 && axl_strcmp(buf, "\x1b[1;31r") == 0,
               "sshd-bind fit: a larger terminal scrolls in the console's rows");
    n = sshd_fit_prefix(buf, sizeof(buf), SSHD_FIT_EXACT, 31, true);
    test_check(n == 3 && axl_strcmp(buf, "\x1b[r") == 0,
               "sshd-bind fit: back to the console's size resets the region");
    n = sshd_fit_prefix(buf, sizeof(buf), SSHD_FIT_EXACT, 31, false);
    test_check(n == 0 && buf[0] == '\0', "sshd-bind fit: same size, no region: nothing");
    n = sshd_fit_prefix(buf, 4, SSHD_FIT_LARGER, 31, false);
    test_check(n == 0, "sshd-bind fit: a buffer too small writes nothing");

    n = sshd_held_notice(buf, sizeof(buf), 100, 31, 80, 24);
    test_check(n > 0 && axl_strcmp(buf,
                   "\x1b[r\x1b[m\x1b[2J\x1b[H"
                   "[sshd] this terminal is 80x24; the console is 100x31.\r\n"
                   "[sshd] enlarge it to at least 100x31 to see the console.\r\n"
                   "[sshd] what you type still reaches the console.\r\n") == 0,
               "sshd-bind fit: the held notice, exactly");
    test_check(n == axl_strlen(buf), "sshd-bind fit: the notice's length");
    test_check(sshd_held_notice(buf, 40, 100, 31, 80, 24) == 0,
               "sshd-bind fit: a notice that does not fit is not written");
}

/* The no-way-to-log-in rule, over all sixteen inputs: open exactly when
   (pubkey && keys) || (pwauth && password). */
static void
test_can_log_in(void)
{
    int wrong = 0;
    int open  = 0;

    for (unsigned m = 0; m < 16; m++) {
        bool pk = (m & 1u) != 0, keys = (m & 2u) != 0;
        bool pa = (m & 4u) != 0, pw = (m & 8u) != 0;
        bool want = (pk && keys) || (pa && pw);
        bool got  = sshd_bind_can_log_in(pk, keys, pa, pw);
        wrong += got != want ? 1 : 0;
        open  += got ? 1 : 0;
    }
    test_check(wrong == 0, "sshd-bind login: the rule holds for all 16 inputs");
    test_check(open == 7, "sshd-bind login: exactly 7 of 16 are open");
    test_check(!sshd_bind_can_log_in(false, true, false, true),
               "sshd-bind login: keys and a password, both methods off: closed");
    test_check(!sshd_bind_can_log_in(true, false, true, false),
               "sshd-bind login: both methods on, nothing set: closed");
    test_check(sshd_bind_can_log_in(false, false, true, true),
               "sshd-bind login: a password alone: open");
}

/* The kept log's append: whole lines, the oldest dropped on a line start,
   never more than the buffer. Exact bytes and lengths. */
static void
test_log_append(void)
{
    char   b[17];   /* cap 16, one guard byte */
    size_t n;

    axl_memset(b, '#', sizeof(b));
    n = sshd_bind_log_append(b, 16, 0, "a\n", 2);
    test_check(n == 2 && axl_memcmp(b, "a\n", 2) == 0 && b[2] == '#',
               "log append: into an empty buffer");

    axl_memset(b, '#', sizeof(b));
    axl_memcpy(b, "aaaa\nbbbb\n", 10);
    n = sshd_bind_log_append(b, 16, 10, "ccccc\n", 6);
    test_check(n == 16 && axl_memcmp(b, "aaaa\nbbbb\nccccc\n", 16) == 0 && b[16] == '#',
               "log append: a line that fits exactly drops nothing");

    axl_memset(b, '#', sizeof(b));
    axl_memcpy(b, "aaaa\nbbbb\ncccc\n", 15);
    n = sshd_bind_log_append(b, 16, 15, "dd\n", 3);
    test_check(n == 8 && axl_memcmp(b, "cccc\ndd\n", 8) == 0 && b[16] == '#',
               "log append: full, the older half goes, whole lines, newest kept");

    axl_memset(b, '#', sizeof(b));
    axl_memcpy(b, "xxxxxxxxxxxxxx\n", 15);
    n = sshd_bind_log_append(b, 16, 15, "y\n", 2);
    test_check(n == 2 && axl_memcmp(b, "y\n", 2) == 0 && b[16] == '#',
               "log append: no line start past the half, so everything older goes");

    axl_memset(b, '#', sizeof(b));
    axl_memcpy(b, "aa\n", 3);
    n = sshd_bind_log_append(b, 16, 3, "zzzzzzzzzzzzzzzzz\n", 18);
    test_check(n == 3 && axl_memcmp(b, "aa\n", 3) == 0 && b[16] == '#',
               "log append: a line longer than the buffer is not kept");

    /* One append is one line: `gen` counts appends, so a message carrying
       its own line breaks must not become two lines in the text. */
    axl_memset(b, '#', sizeof(b));
    n = sshd_bind_log_append(b, 16, 0, "a\nb\r\nc\n", 7);
    test_check(n == 7 && axl_memcmp(b, "a b  c\n", 7) == 0 && b[7] == '#',
               "log append: a line's own breaks become spaces");
}

/* The refused stop's reason: the newest [ERROR] among the lines written
   after the stop began. Exact lines. */
static void
test_newest_error(void)
{
    static const char some[] = "[INFO] ssh: a\n[ERROR] sshd: one\n[DEBUG] x: y\n"
                               "[ERROR] sshd: two\n[INFO] ssh: b\n";
    static const char open[] = "[ERROR] sshd: one\n[ERROR] sshd: last";
    const char *l = NULL;
    size_t      ll = 99;

    test_check(!sshd_bind_newest_error("", 0, 10, &l, &ll),
               "newest error: empty text has none");
    test_check(!sshd_bind_newest_error("[INFO] a: b\n[DEBUG] c: d\n", 25, 10, &l, &ll),
               "newest error: no ERROR line, none");
    test_check(sshd_bind_newest_error("[INFO] a: b\n[ERROR] sshd: bad\n", 30, 10, &l, &ll)
                   && ll == 17 && axl_strncmp(l, "[ERROR] sshd: bad", 17) == 0,
               "newest error: the one ERROR line, without its newline");
    test_check(sshd_bind_newest_error(some, sizeof(some) - 1, 99, &l, &ll)
                   && ll == 17 && axl_strncmp(l, "[ERROR] sshd: two", 17) == 0,
               "newest error: of several, the newest");
    test_check(sshd_bind_newest_error(open, sizeof(open) - 1, 99, &l, &ll)
                   && ll == 18 && axl_strncmp(l, "[ERROR] sshd: last", 18) == 0,
               "newest error: a last line with no newline");
    test_check(!sshd_bind_newest_error(some, sizeof(some) - 1, 1, &l, &ll),
               "newest error: an ERROR older than the last N lines is not chosen");
    test_check(sshd_bind_newest_error(some, sizeof(some) - 1, 2, &l, &ll)
                   && ll == 17 && axl_strncmp(l, "[ERROR] sshd: two", 17) == 0,
               "newest error: one inside the last N lines is");
    test_check(!sshd_bind_newest_error(some, sizeof(some) - 1, 0, &l, &ll),
               "newest error: no line written since, none");
}

/* AxlLogPort: auto takes the first port the console does not draw on, and
   none such means no log; a number is honoured as asked, console or not,
   and refused when the machine lacks it. */
static void
test_log_port(void)
{
    size_t at = 99;

    static const bool all_console[2] = { true, true };
    static const bool second_free[3] = { true, false, false };
    static const bool first_free[2]  = { false, true };

    test_check(sshd_bind_log_port_pick(SSHD_LOG_PORT_AUTO, second_free, 3, &at)
                   == SSHD_LOG_TO_PORT && at == 1,
               "log port pick: auto takes the FIRST port the console is not on");
    at = 99;
    test_check(sshd_bind_log_port_pick(SSHD_LOG_PORT_AUTO, first_free, 2, &at)
                   == SSHD_LOG_TO_PORT && at == 0,
               "log port pick: auto takes port 0 when the console is not on it");
    at = 99;
    test_check(sshd_bind_log_port_pick(SSHD_LOG_PORT_AUTO, all_console, 2, &at)
                   == SSHD_LOG_OFF_CONSOLE && at == 99,
               "log port pick: auto with the console on every port writes nothing");
    test_check(sshd_bind_log_port_pick(SSHD_LOG_PORT_AUTO, NULL, 0, &at)
                   == SSHD_LOG_OFF_CONSOLE && at == 99,
               "log port pick: auto with no port writes nothing");
    test_check(sshd_bind_log_port_pick(SSHD_LOG_PORT_NONE, first_free, 2, &at)
                   == SSHD_LOG_OFF_NONE && at == 99,
               "log port pick: none writes nothing, even with a free port");
    test_check(sshd_bind_log_port_pick(0, all_console, 2, &at)
                   == SSHD_LOG_TO_PORT && at == 0,
               "log port pick: a number is honoured on the console's own port");
    at = 99;
    test_check(sshd_bind_log_port_pick(2, all_console, 2, &at)
                   == SSHD_LOG_NO_SUCH_PORT && at == 99,
               "log port pick: a number past the last port is refused");
}

/* sshd's own log while it serves: the serial port if there is one, else
   nowhere -- and afterwards whatever it was before, not always on. A stop
   that never moved the log (setup failed before it did, the image stayed
   resident, `sshd stop` later) leaves it as it is now. */
static void
test_log_console(void)
{
    test_check(sshd_bind_log_console(true, false, true, true, true),
               "sshd-bind log: serving with a serial port: written (to the port)");
    test_check(!sshd_bind_log_console(true, false, false, true, true),
               "sshd-bind log: serving with no serial port: not written");
    test_check(!sshd_bind_log_console(true, false, true, false, false),
               "sshd-bind log: serving, but it was off before: stays off");
    test_check(sshd_bind_log_console(false, true, true, true, false)
                   && sshd_bind_log_console(false, true, false, true, false),
               "sshd-bind log: stopped, it was on: back on");
    test_check(!sshd_bind_log_console(false, true, true, false, true)
                   && !sshd_bind_log_console(false, true, false, false, true),
               "sshd-bind log: stopped, it was off: stays off");
    test_check(sshd_bind_log_console(false, false, true, false, true)
                   && sshd_bind_log_console(false, false, false, false, true),
               "sshd-bind log: false without a prior true, it is on: stays on");
    test_check(!sshd_bind_log_console(false, false, true, true, false)
                   && !sshd_bind_log_console(false, false, false, true, false),
               "sshd-bind log: false without a prior true, it is off: stays off");
}

/* A Banner file is read as sshd.conf is: UTF-8 with or without a BOM, or
   UTF-16LE + BOM as the Shell writes it; the text must be UTF-8 without a
   NUL, and the limit is on the text. */
static void
test_banner_text(void)
{
    char  *t = NULL;
    size_t n = 0;

    test_check(sshd_bind_banner_text("Hi\r\n", 4, 100, &t, &n) == SSHD_BANNER_OK
                   && t != NULL && axl_strcmp(t, "Hi\r\n") == 0 && n == 4,
               "sshd-bind banner text: UTF-8");
    axl_free(t);
    test_check(sshd_bind_banner_text("\xEF\xBB\xBFHi\r\n", 7, 100, &t, &n) == SSHD_BANNER_OK
                   && t != NULL && axl_strcmp(t, "Hi\r\n") == 0 && n == 4,
               "sshd-bind banner text: UTF-8 with a BOM, the BOM dropped");
    axl_free(t);
    static const unsigned char u16[] = { 0xFF, 0xFE, 'H', 0, 'i', 0, 0xE9, 0, '\r', 0, '\n', 0 };
    test_check(sshd_bind_banner_text(u16, sizeof(u16), 100, &t, &n) == SSHD_BANNER_OK
                   && t != NULL && axl_strcmp(t, "Hi\xC3\xA9\r\n") == 0 && n == 6,
               "sshd-bind banner text: UTF-16LE + BOM, as the Shell writes it");
    axl_free(t);

    static const unsigned char lone[] = { 0xFF, 0xFE, 'H', 0, 0x00, 0xD8, '\n', 0 };
    test_check(sshd_bind_banner_text(lone, sizeof(lone), 100, &t, &n) == SSHD_BANNER_NOT_TEXT
                   && t == NULL,
               "sshd-bind banner text: an unpaired surrogate is not text");
    static const unsigned char nul16[] = { 0xFF, 0xFE, 'H', 0, 0, 0, 'i', 0 };
    test_check(sshd_bind_banner_text(nul16, sizeof(nul16), 100, &t, &n) == SSHD_BANNER_NOT_TEXT
                   && t == NULL,
               "sshd-bind banner text: an encoded NUL is not text");
    test_check(sshd_bind_banner_text("caf\xe9\r\n", 6, 100, &t, &n) == SSHD_BANNER_NOT_TEXT
                   && t == NULL,
               "sshd-bind banner text: Latin-1 is not text");
    test_check(sshd_bind_banner_text("a\0b", 3, 100, &t, &n) == SSHD_BANNER_NOT_TEXT
                   && t == NULL,
               "sshd-bind banner text: a NUL byte is not text");

    test_check(sshd_bind_banner_text("abcd", 4, 4, &t, &n) == SSHD_BANNER_OK && n == 4,
               "sshd-bind banner text: exactly the limit is sent");
    axl_free(t);
    test_check(sshd_bind_banner_text("abcde", 5, 4, &t, &n) == SSHD_BANNER_TOO_LARGE
                   && t == NULL,
               "sshd-bind banner text: one byte over the limit is too large");
    /* Ten bytes of UTF-16 file, four of text: the limit is on the text. */
    static const unsigned char u16_4[] = { 0xFF, 0xFE, 'a', 0, 'b', 0, 'c', 0, 'd', 0 };
    test_check(sshd_bind_banner_text(u16_4, sizeof(u16_4), 4, &t, &n) == SSHD_BANNER_OK
                   && n == 4,
               "sshd-bind banner text: the limit is on the text, not the UTF-16 file");
    axl_free(t);
}

/* The Banner file's text, then the console-size line sshd always sends. */
static void
test_banner_compose(void)
{
    static const char geom[] =
        "sshd: the console is 80x25; a smaller terminal waits until it is enlarged\n";
    char   buf[256];
    size_t n;

    n = sshd_banner_compose(buf, sizeof(buf), "Authorized use only.\n", 21, 80, 25);
    test_check(axl_strcmp(buf, "Authorized use only.\n"
                               "sshd: the console is 80x25; a smaller terminal waits "
                               "until it is enlarged\n") == 0
               && n == axl_strlen(buf),
               "sshd-bind banner: the file's text, then the size line");
    n = sshd_banner_compose(buf, sizeof(buf), "No newline", 10, 80, 25);
    test_check(axl_strcmp(buf, "No newline\n"
                               "sshd: the console is 80x25; a smaller terminal waits "
                               "until it is enlarged\n") == 0
               && n == axl_strlen(buf),
               "sshd-bind banner: a newline is added when the text has none");
    n = sshd_banner_compose(buf, sizeof(buf), "", 0, 80, 25);
    test_check(axl_strcmp(buf, geom) == 0 && n == sizeof(geom) - 1,
               "sshd-bind banner: no Banner is the size line alone");
    n = sshd_banner_compose(buf, sizeof(buf), NULL, 0, 80, 25);
    test_check(axl_strcmp(buf, geom) == 0 && n == sizeof(geom) - 1,
               "sshd-bind banner: a NULL Banner is the size line alone");
    n = sshd_banner_compose(buf, sizeof(buf), "Only the first five", 5, 80, 25);
    test_check(axl_strcmp(buf, "Only \n"
                               "sshd: the console is 80x25; a smaller terminal waits "
                               "until it is enlarged\n") == 0,
               "sshd-bind banner: exactly user_len bytes of the text");
    /* Exactly fits (with its NUL), then one byte short. */
    n = sshd_banner_compose(buf, sizeof(geom), "", 0, 80, 25);
    test_check(n == sizeof(geom) - 1 && axl_strcmp(buf, geom) == 0,
               "sshd-bind banner: a buffer of exactly the size fits");
    buf[0] = 'x';
    n = sshd_banner_compose(buf, sizeof(geom) - 1, "", 0, 80, 25);
    test_check(n == 0 && buf[0] == '\0', "sshd-bind banner: one byte short writes nothing");
    buf[0] = 'x';
    n = sshd_banner_compose(buf, 30, "Authorized use only.\n", 21, 80, 25);
    test_check(n == 0 && buf[0] == '\0',
               "sshd-bind banner: a text that does not fit writes nothing");
}

/* OpenSSH's LogLevel onto AxlLog's five levels. Not given: nothing is set,
   so AXL_LOG_LEVEL in the Shell still decides. QUIET and FATAL still log
   errors (a setup failure must explain itself); VERBOSE is DEBUG (as INFO
   it would change nothing); DEBUG2/DEBUG3 are TRACE. */
static void
test_log_level(void)
{
    static const struct { uint32_t in; int want; const char *name; } map[] = {
        { SSHD_LOG_UNSET,   -1,              "not given: leave AxlLog alone" },
        { SSHD_LOG_QUIET,   AXL_LOG_ERROR,   "QUIET is ERROR" },
        { SSHD_LOG_FATAL,   AXL_LOG_ERROR,   "FATAL is ERROR" },
        { SSHD_LOG_ERROR,   AXL_LOG_ERROR,   "ERROR is ERROR" },
        { SSHD_LOG_INFO,    AXL_LOG_INFO,    "INFO is INFO" },
        { SSHD_LOG_VERBOSE, AXL_LOG_DEBUG,   "VERBOSE is DEBUG" },
        { SSHD_LOG_DEBUG1,  AXL_LOG_DEBUG,   "DEBUG1 is DEBUG" },
        { SSHD_LOG_DEBUG2,  AXL_LOG_TRACE,   "DEBUG2 is TRACE" },
        { SSHD_LOG_DEBUG3,  AXL_LOG_TRACE,   "DEBUG3 is TRACE" },
        { 99,               -1,              "out of range: leave AxlLog alone" },
    };
    for (size_t i = 0; i < AXL_ARRAY_SIZE(map); i++) {
        char name[96];
        axl_snprintf(name, sizeof(name), "sshd-bind loglevel: %s", map[i].name);
        test_check(sshd_bind_log_level(map[i].in) == map[i].want, name);
    }
}

/* Where sshd says it serves: whole addresses, ", "-joined; "0.0.0.0" for
   none; past the buffer, "+N more" rather than a cut address. */
static void
test_addr_list(void)
{
    static const char *const two[]  = { "10.0.2.15", "10.0.3.15" };
    static const char *const five[] = { "10.0.2.15", "10.0.3.15", "192.168.100.200",
                                        "10.0.4.15", "10.0.5.15" };
    char out[SSHD_ADDRS_MAX];
    char small[32];

    sshd_bind_addr_list(out, sizeof(out), NULL, 0);
    test_check(axl_strcmp(out, "0.0.0.0") == 0, "sshd-bind addrs: none is 0.0.0.0");
    sshd_bind_addr_list(out, sizeof(out), two, 1);
    test_check(axl_strcmp(out, "10.0.2.15") == 0, "sshd-bind addrs: one");
    sshd_bind_addr_list(out, sizeof(out), two, 2);
    test_check(axl_strcmp(out, "10.0.2.15, 10.0.3.15") == 0, "sshd-bind addrs: two, comma-joined");
    /* 32 bytes: two whole addresses (20) leave too little for a third and
       its " +N more"; the third onward are counted, never cut. */
    sshd_bind_addr_list(small, sizeof(small), five, 5);
    test_check(axl_strcmp(small, "10.0.2.15, 10.0.3.15, +3 more") == 0,
               "sshd-bind addrs: past the buffer, +N more");
    sshd_bind_addr_list(out, sizeof(out), five, 5);
    test_check(axl_strcmp(out, "10.0.2.15, 10.0.3.15, 192.168.100.200, 10.0.4.15, 10.0.5.15") == 0,
               "sshd-bind addrs: five fit SSHD_ADDRS_MAX");
}

/* The HostKey list the launcher hands the driver: SSHD_HOSTKEY_SEP-joined,
   walked in order, an empty list holding nothing. */
static void
test_hostkey_list(void)
{
    const char *cur = "fs0:\\a|none|fs0:\\b";
    char        p[16];
    bool        ok = sshd_bind_hostkey_next(&cur, p, sizeof p)
                     && axl_strcmp(p, "fs0:\\a") == 0;
    ok = ok && sshd_bind_hostkey_next(&cur, p, sizeof p) && axl_strcmp(p, "none") == 0;
    ok = ok && sshd_bind_hostkey_next(&cur, p, sizeof p) && axl_strcmp(p, "fs0:\\b") == 0;
    ok = ok && !sshd_bind_hostkey_next(&cur, p, sizeof p);
    test_check(ok, "hostkey list: three entries in order, then the end");

    cur = "";
    test_check(!sshd_bind_hostkey_next(&cur, p, sizeof p),
               "hostkey list: an empty list (the stored key) has no entry");

    cur = "fs0:\\a-very-long-name|x";
    test_check(sshd_bind_hostkey_next(&cur, p, 8) && axl_strcmp(p, "fs0:\\a-") == 0
               && sshd_bind_hostkey_next(&cur, p, sizeof p) && axl_strcmp(p, "x") == 0,
               "hostkey list: a long entry is cut and still consumed whole");
}

static int
test_sshd_bind_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("SshdBind");

    test_plain_text();
    test_escape_sequences();
    test_hold_is_observable();
    test_discard();
    test_utf8_validity();
    test_esc_timeout();
    test_utf8();
    test_enter();
    test_control_keys();
    test_paste_lf_is_enter();
    test_esc_alone_then_letter();
    test_input_isolation();
    test_sessions();
    test_session_input_state();
    test_repaint();
    test_deferral();
    test_fan_write();
    test_release();
    test_goodbye_text();
    test_banner_text();
    test_pend();
    test_route();
    test_ctrl_waits_for_reads();
    test_con_drop();
    test_setup_failed();
    test_ratelimit();
    test_fit();
    test_banner_compose();
    test_can_log_in();
    test_log_console();
    test_log_port();
    test_log_append();
    test_newest_error();
    test_log_level();
    test_addr_list();
    test_hostkey_list();

    return test_print_results();
}

AXL_APP(test_sshd_bind_main)
