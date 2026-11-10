/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * AxlTestSshdSftp -- the SFTP v3 codec and path normaliser
 * (tools/sshd-sftp-codec.c), pure functions, every output compared exactly.
 */

#include <axl.h>
#include "axl-test.h"
#include "../../tools/sshd-sftp-codec.h"
#include "../../tools/sshd-sftp.h"
#include "../../tools/sshd-sftp-resume.h"

/* ---- helpers --------------------------------------------------------------- */

/* Hex of @a n bytes into a static buffer. */
static const char *
hex(const uint8_t *p, size_t n)
{
    static char out[1024];
    static const char D[] = "0123456789abcdef";
    size_t k = 0;

    for (size_t i = 0; i < n && k + 3 < sizeof(out); i++) {
        out[k++] = D[p[i] >> 4];
        out[k++] = D[p[i] & 15];
    }
    out[k] = '\0';
    return out;
}

/* Hex string into bytes; returns the count. */
static size_t
unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = 0;

    while (h[0] != '\0' && h[1] != '\0' && n < cap) {
        uint32_t v = 0;
        char     t[3] = { h[0], h[1], '\0' };
        if (axl_str_to_u32(t, 16, &v, NULL) != AXL_OK) {
            break;
        }
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

static bool
str_is(SftpStr s, const char *want)
{
    size_t n = axl_strlen(want);
    return s.p != NULL && s.len == n && axl_memcmp(s.p, want, n) == 0;
}

/* ---- framing -------------------------------------------------------------- */

static void
test_rx_chunks(void)
{
    SftpRx         rx   = { 0 };
    const uint8_t *body = NULL;
    uint32_t       blen = 0;
    static const uint8_t A[] = { 0, 0, 0, 5, 1, 0, 0, 0, 3 };

    test_check(sftp_rx_feed(&rx, A, 2) == AXL_OK, "rx: two length bytes fed");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_INCOMPLETE,
               "rx: half a length is incomplete");
    test_check(sftp_rx_feed(&rx, A + 2, 4) == AXL_OK, "rx: more fed");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_INCOMPLETE,
               "rx: part of the body is incomplete");
    test_check(sftp_rx_feed(&rx, A + 6, 3) == AXL_OK, "rx: the rest fed");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_OK && blen == 5
                   && axl_strcmp(hex(body, blen), "0100000003") == 0,
               "rx: one whole packet, its body without the length");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_INCOMPLETE,
               "rx: nothing after it");
    sftp_rx_free(&rx);
    test_check(rx.buf == NULL && rx.len == 0, "rx: free zeroes it");
}

static void
test_rx_two_in_one(void)
{
    SftpRx         rx   = { 0 };
    const uint8_t *body = NULL;
    uint32_t       blen = 0;
    uint8_t        b[32];
    size_t         n = unhex("00000001aa00000002bbcc000000", b, sizeof(b));

    test_check(sftp_rx_feed(&rx, b, n) == AXL_OK, "rx2: fed");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_OK
                   && axl_strcmp(hex(body, blen), "aa") == 0,
               "rx2: the first packet");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_OK
                   && axl_strcmp(hex(body, blen), "bbcc") == 0,
               "rx2: the second packet");
    test_check(sftp_rx_next(&rx, &body, &blen) == AXL_INCOMPLETE,
               "rx2: a third's partial length waits");
    test_check(sftp_rx_feed(&rx, (const uint8_t *)"\x01\x07", 2) == AXL_OK
                   && sftp_rx_next(&rx, &body, &blen) == AXL_OK
                   && axl_strcmp(hex(body, blen), "07") == 0,
               "rx2: completing it after compaction");
    sftp_rx_free(&rx);
}

static void
test_rx_bounds(void)
{
    SftpRx         rx   = { 0 };
    const uint8_t *body = NULL;
    uint32_t       blen = 0;
    uint8_t        len[4];

    test_check(sftp_rx_feed(&rx, (const uint8_t *)"\0\0\0\0", 4) == AXL_OK
                   && sftp_rx_next(&rx, &body, &blen) == AXL_INVALID,
               "rx bounds: a zero length is refused");
    sftp_rx_free(&rx);

    /* One past the bound, with only the length held: refused at once, and
       nothing the size of the packet was allocated for it. */
    uint32_t over = SFTP_PACKET_MAX + 1u;
    len[0] = (uint8_t)(over >> 24); len[1] = (uint8_t)(over >> 16);
    len[2] = (uint8_t)(over >> 8);  len[3] = (uint8_t)over;
    test_check(sftp_rx_feed(&rx, len, 4) == AXL_OK
                   && sftp_rx_next(&rx, &body, &blen) == AXL_INVALID,
               "rx bounds: SFTP_PACKET_MAX + 1 is refused from its length alone");
    test_check(rx.cap < 4096, "rx bounds: ...before anything packet-sized was allocated");
    sftp_rx_free(&rx);

    uint32_t at = SFTP_PACKET_MAX;
    len[0] = (uint8_t)(at >> 24); len[1] = (uint8_t)(at >> 16);
    len[2] = (uint8_t)(at >> 8);  len[3] = (uint8_t)at;
    test_check(sftp_rx_feed(&rx, len, 4) == AXL_OK
                   && sftp_rx_next(&rx, &body, &blen) == AXL_INCOMPLETE,
               "rx bounds: exactly SFTP_PACKET_MAX is accepted (waits for its body)");
    sftp_rx_free(&rx);

    /* The hold limit: SFTP_RX_MAX bytes are held, one more is refused. */
    uint8_t *chunk = axl_calloc(1, 64u * 1024u);
    size_t   fed   = 0;
    int      rc    = AXL_OK;
    while (chunk != NULL && rc == AXL_OK && fed < SFTP_RX_MAX) {
        size_t n = SFTP_RX_MAX - fed < 64u * 1024u ? SFTP_RX_MAX - fed : 64u * 1024u;
        rc = sftp_rx_feed(&rx, chunk, n);
        fed += n;
    }
    test_check(rc == AXL_OK && fed == SFTP_RX_MAX,
               "rx bounds: SFTP_RX_MAX bytes can be held");
    test_check(chunk != NULL && sftp_rx_feed(&rx, chunk, 1) == AXL_NO_RESOURCES,
               "rx bounds: one byte more is refused");
    axl_free(chunk);
    sftp_rx_free(&rx);
}

/* ---- decoding ------------------------------------------------------------- */

/* Decode the hex body @a h into @a r. */
static int
dec(const char *h, SftpRequest *r)
{
    static uint8_t b[2048];
    size_t n = unhex(h, b, sizeof(b));

    return sftp_decode(b, n, r);
}

/* str("x") as hex. */
#define S_PATH   "000000062f6673302f61"           /* "/fs0/a" */
#define S_PATH2  "000000062f6673302f62"           /* "/fs0/b" */
#define S_HANDLE "000000080000000300000009"       /* index 3, gen 9 */
#define ATTRS_0  "00000000"

static void
test_decode_types(void)
{
    SftpRequest r;

    test_check(dec("0100000003", &r) == AXL_OK && r.type == SFTP_INIT && r.version == 3,
               "decode: INIT version 3");
    test_check(dec("03" "00000011" S_PATH "00000019" ATTRS_0, &r) == AXL_OK
                   && r.type == SFTP_OPEN && r.id == 17 && str_is(r.path, "/fs0/a")
                   && r.pflags == 0x19 && r.attrs.flags == 0,
               "decode: OPEN id, path, pflags, attrs");
    test_check(dec("04" "00000001" S_HANDLE, &r) == AXL_OK && r.type == SFTP_CLOSE
                   && r.handle.len == 8,
               "decode: CLOSE handle");
    test_check(dec("05" "00000002" S_HANDLE "0000000100000002" "00008000", &r) == AXL_OK
                   && r.type == SFTP_READ && r.offset == 0x100000002ull
                   && r.length == 0x8000,
               "decode: READ offset (64-bit) and length");
    test_check(dec("06" "00000003" S_HANDLE "0000000000000010" "00000003616263", &r) == AXL_OK
                   && r.type == SFTP_WRITE && r.offset == 16 && str_is(r.data, "abc"),
               "decode: WRITE offset and data");
    static const uint8_t PATH_TYPES[] = {
        SFTP_LSTAT, SFTP_STAT, SFTP_OPENDIR, SFTP_REMOVE, SFTP_RMDIR,
        SFTP_REALPATH, SFTP_READLINK,
    };
    for (size_t i = 0; i < sizeof(PATH_TYPES); i++) {
        char h[64];
        axl_snprintf(h, sizeof(h), "%02x00000004" S_PATH, PATH_TYPES[i]);
        test_check(dec(h, &r) == AXL_OK && r.type == PATH_TYPES[i] && r.id == 4
                       && str_is(r.path, "/fs0/a"),
                   "decode: a path request");
    }
    static const uint8_t HANDLE_TYPES[] = { SFTP_FSTAT, SFTP_READDIR };
    for (size_t i = 0; i < sizeof(HANDLE_TYPES); i++) {
        char h[64];
        axl_snprintf(h, sizeof(h), "%02x00000005" S_HANDLE, HANDLE_TYPES[i]);
        test_check(dec(h, &r) == AXL_OK && r.type == HANDLE_TYPES[i]
                       && r.handle.len == 8,
                   "decode: a handle request");
    }
    test_check(dec("09" "00000006" S_PATH "00000001" "0000000000000400", &r) == AXL_OK
                   && r.type == SFTP_SETSTAT && r.attrs.flags == SFTP_ATTR_SIZE
                   && r.attrs.size == 1024,
               "decode: SETSTAT path and a size attribute");
    test_check(dec("0e" "00000007" S_PATH "00000004" "000001ed", &r) == AXL_OK
                   && r.type == SFTP_MKDIR && r.attrs.perm == 0755,
               "decode: MKDIR path and permissions");
    test_check(dec("0a" "00000008" S_HANDLE "00000008" "0000000100000002", &r) == AXL_OK
                   && r.type == SFTP_FSETSTAT && r.attrs.atime == 1 && r.attrs.mtime == 2,
               "decode: FSETSTAT handle and times");
    test_check(dec("12" "00000009" S_PATH S_PATH2, &r) == AXL_OK && r.type == SFTP_RENAME
                   && str_is(r.path, "/fs0/a") && str_is(r.path2, "/fs0/b"),
               "decode: RENAME old and new");
    test_check(dec("14" "0000000a" S_PATH S_PATH2, &r) == AXL_OK && r.type == SFTP_SYMLINK
                   && str_is(r.path2, "/fs0/b"),
               "decode: SYMLINK");
    test_check(dec("c8" "0000000b" "000000066c696d697473" "dead", &r) == AXL_OK
                   && r.type == SFTP_EXTENDED && str_is(r.ext, "limits")
                   && r.data.len == 2,
               "decode: EXTENDED name and the rest as data");
    SftpStr a = { NULL, 0 };
    SftpStr b = { NULL, 0 };
    test_check(dec("c8" "0000000e" "00000004" "61626364" S_PATH S_PATH2 "ff", &r) == AXL_OK
                   && sftp_ext_two_paths(&r, &a, &b) == AXL_OK && str_is(a, "/fs0/a")
                   && str_is(b, "/fs0/b"),
               "decode: an EXTENDED request's two path arguments, a trailing byte ignored");
    bool cut_all = true;
    static const char *const CUT[] = {
        "c8" "0000000e" "00000004" "61626364",
        "c8" "0000000e" "00000004" "61626364" S_PATH,
        "c8" "0000000e" "00000004" "61626364" S_PATH "00000006" "2f6673302f",
        "c8" "0000000e" "00000004" "61626364" "00000006",
    };
    for (size_t i = 0; i < AXL_ARRAY_SIZE(CUT); i++) {
        cut_all = dec(CUT[i], &r) == AXL_OK && sftp_ext_two_paths(&r, &a, &b) == AXL_INVALID
                  && cut_all;
    }
    test_check(cut_all, "decode: no strings, one, a second cut short, a first cut short: "
                        "each AXL_INVALID");
    test_check(dec("11" "0000000f" S_PATH, &r) == AXL_OK
                   && sftp_ext_two_paths(&r, &a, &b) == AXL_INVALID,
               "decode: two paths from a request that is not EXTENDED is AXL_INVALID");
    test_check(dec("63" "0000000c", &r) == AXL_OK && r.type == 99 && r.id == 12,
               "decode: an unknown type decodes with its id");
    test_check(dec("11" "0000000d" S_PATH "ffff", &r) == AXL_OK && r.type == SFTP_STAT,
               "decode: trailing bytes are ignored, as sftp-server discards them");
}

static void
test_decode_attrs(void)
{
    SftpRequest r;

    /* Every flag, with two extended pairs. */
    test_check(dec("09" "00000001" S_PATH "8000000f" "0000000000000005" "0000000a0000000b"
                   "000081a4" "0000000c0000000d" "00000002"
                   "000000016100000000" "000000016200000001" "63", &r) == AXL_OK
                   && r.attrs.size == 5 && r.attrs.uid == 10 && r.attrs.gid == 11
                   && r.attrs.perm == 0100644 && r.attrs.atime == 12 && r.attrs.mtime == 13,
               "attrs: every flag, two extended pairs");
    /* SFTP_ATTR_EXT_MAX well-formed (empty) pairs parse; one more is
       refused even though every pair is there. */
    for (uint32_t n = SFTP_ATTR_EXT_MAX; n <= SFTP_ATTR_EXT_MAX + 1u; n++) {
        static char h[64 + 16u * (SFTP_ATTR_EXT_MAX + 1u) + 8];
        size_t      k = (size_t)axl_snprintf(h, sizeof(h), "09" "00000001" S_PATH
                                             "80000000" "%08x", (unsigned)n);
        for (uint32_t i = 0; i < n; i++) {
            axl_memcpy(h + k, "0000000000000000", 16);
            k += 16;
        }
        h[k] = '\0';
        int rc = dec(h, &r);
        test_check(n == SFTP_ATTR_EXT_MAX ? rc == AXL_OK : rc == AXL_INVALID,
                   n == SFTP_ATTR_EXT_MAX ? "attrs: SFTP_ATTR_EXT_MAX extended pairs parse"
                                          : "attrs: one pair more is refused");
    }
    test_check(dec("09" "00000001" S_PATH "00000001" "00000000", &r) == AXL_INVALID,
               "attrs: a size cut short is refused");
}

/* Every proper prefix of a valid request (past its type byte) fails. */
static void
test_decode_truncated(void)
{
    static const char *const OK[] = {
        "0100000003",
        "03" "00000011" S_PATH "00000019" ATTRS_0,
        "05" "00000002" S_HANDLE "0000000100000002" "00008000",
        "06" "00000003" S_HANDLE "0000000000000010" "00000003616263",
        "12" "00000009" S_PATH S_PATH2,
        "0a" "00000008" S_HANDLE "00000008" "0000000100000002",
        "c8" "0000000b" "000000066c696d697473",
    };
    static uint8_t b[256];
    bool all = true;

    for (size_t i = 0; i < AXL_ARRAY_SIZE(OK); i++) {
        SftpRequest r;
        size_t n = unhex(OK[i], b, sizeof(b));
        if (sftp_decode(b, n, &r) != AXL_OK) {
            all = false;
        }
        for (size_t cut = 0; cut < n; cut++) {
            if (sftp_decode(b, cut, &r) != AXL_INVALID) {
                all = false;
            }
        }
    }
    test_check(all, "decode: every proper prefix of each request is refused");
}

static void
test_decode_bounds(void)
{
    static uint8_t b[2200];
    SftpRequest r;

    /* A path longer than any the server serves still parses: the session
       answers it (BAD_MESSAGE, as sftp-server's ENAMETOOLONG), rather than
       ending. The packet alone bounds it. */
    {
        uint32_t n = 2000;
        b[0] = SFTP_STAT;
        b[1] = 0; b[2] = 0; b[3] = 0; b[4] = 1;
        b[5] = (uint8_t)(n >> 24); b[6] = (uint8_t)(n >> 16);
        b[7] = (uint8_t)(n >> 8);  b[8] = (uint8_t)n;
        axl_memset(b + 9, 'a', n);
        test_check(sftp_decode(b, 9 + n, &r) == AXL_OK && r.path.len == 2000,
                   "decode bounds: a 2000-byte path parses, for the session to answer");
    }
    /* A 257-byte handle is refused; 256 parses. */
    for (uint32_t n = 256; n <= 257; n++) {
        b[0] = SFTP_CLOSE;
        b[1] = 0; b[2] = 0; b[3] = 0; b[4] = 1;
        b[5] = 0; b[6] = 0; b[7] = (uint8_t)(n >> 8); b[8] = (uint8_t)n;
        axl_memset(b + 9, 'h', n);
        int rc = sftp_decode(b, 9 + n, &r);
        test_check(n == 256 ? rc == AXL_OK : rc == AXL_INVALID,
                   n == 256 ? "decode bounds: a 256-byte handle parses"
                            : "decode bounds: a 257-byte handle is refused");
    }
    test_check(dec("11" "00000001" "ffffffff" "2f", &r) == AXL_INVALID,
               "decode bounds: a string length past the packet is refused");
    test_check(sftp_decode(b, 0, &r) == AXL_INVALID,
               "decode bounds: an empty body is refused");
}

/* ---- encoding ------------------------------------------------------------- */

static void
test_encode(void)
{
    uint8_t  st[512];
    SftpOut  o;

    sftp_out_init(&o, st, sizeof(st));
    sftp_put_status(&o, 7, SFTP_FX_NO_SUCH_FILE, NULL);
    test_check(!o.bad && axl_strcmp(hex(st, o.len),
               "0000001d6500000007000000020000000c4e6f20737563682066696c6500000000") == 0,
               "encode: STATUS no-such-file with sftp-server's text, empty language");

    sftp_out_init(&o, st, sizeof(st));
    sftp_put_status(&o, 9, SFTP_FX_FAILURE, "boom");
    test_check(axl_strcmp(hex(st, o.len),
               "0000001565000000090000000400000004626f6f6d00000000") == 0,
               "encode: STATUS with its own message");

    static const char *const EN[] = { "posix-rename@openssh.com", "limits@openssh.com" };
    static const char *const ED[] = { "1", "1" };
    sftp_out_init(&o, st, sizeof(st));
    sftp_put_version(&o, EN, ED, 2);
    test_check(axl_strcmp(hex(st, o.len),
               "00000041020000000300000018706f7369782d72656e616d65406f70656e7373682e636f6d"
               "0000000131000000126c696d697473406f70656e7373682e636f6d0000000131") == 0,
               "encode: VERSION 3 with two extensions");

    /* What sftp-server's limits reply carries, four uint64s, here with this
       server's own numbers. */
    sftp_out_init(&o, st, sizeof(st));
    sftp_put_limits(&o, 0x11223344u, SFTP_PACKET_MAX, SFTP_READ_MAX, SFTP_WRITE_MAX, 16);
    test_check(!o.bad && axl_strcmp(hex(st, o.len),
               "00000025c911223344" "0000000000040000" "0000000000008000"
               "000000000003fc00" "0000000000000010") == 0,
               "encode: EXTENDED_REPLY to limits: 256 KB packet, 32 KB read, "
               "256 KB - 1024 write, 16 handles");

    uint8_t h[SFTP_HANDLE_LEN];
    sftp_handle_encode(h, 3, 0x01020304u);
    sftp_out_init(&o, st, sizeof(st));
    sftp_put_handle(&o, 5, h, sizeof(h));
    test_check(axl_strcmp(hex(st, o.len), "0000001166000000050000000800000003" "01020304") == 0,
               "encode: HANDLE (index 3, generation 0x01020304)");

    sftp_out_init(&o, st, sizeof(st));
    uint8_t *d = sftp_data_begin(&o, 6, 100);
    test_check(d != NULL, "encode: DATA begin");
    if (d != NULL) {
        axl_memcpy(d, "abc", 3);
    }
    sftp_data_end(&o, 3);
    test_check(axl_strcmp(hex(st, o.len), "0000000c670000000600000003616263") == 0,
               "encode: DATA carrying 3 of 100 bytes asked for");

    SftpAttrs a;
    sftp_attrs_synth(&a, false, false, 1234, 1700000000u);
    sftp_out_init(&o, st, sizeof(st));
    sftp_put_attrs(&o, 8, &a);
    test_check(axl_strcmp(hex(st, o.len),
               "0000001d69000000080000000d00000000000004d2000081a46553f1006553f100") == 0,
               "encode: ATTRS size, permissions, times");

    SftpAttrs dir;
    sftp_attrs_synth(&dir, true, false, 0, 0);
    sftp_out_init(&o, st, sizeof(st));
    sftp_name_begin(&o, 9);
    sftp_name_add(&o, "a", "la", &a);
    sftp_name_add(&o, "dir", "ld", &dir);
    sftp_name_end(&o, 2);
    test_check(axl_strcmp(hex(st, o.len),
               "000000516800000009000000020000000161000000026c610000000d00000000000004d2"
               "000081a46553f1006553f10000000003646972000000026c640000000d00000000000000"
               "00000041ed0000000000000000") == 0,
               "encode: NAME with two entries, the count patched in");

    /* Overflow: nothing past cap, and bad set. */
    uint8_t small[8];
    sftp_out_init(&o, small, sizeof(small));
    sftp_put_status(&o, 1, SFTP_FX_OK, NULL);
    test_check(o.bad, "encode: a reply that does not fit sets bad");
    sftp_out_init(&o, small, sizeof(small));
    test_check(sftp_data_begin(&o, 1, 100) == NULL && o.bad,
               "encode: a DATA reply that cannot fit gives no pointer");
}

static void
test_status_messages(void)
{
    static const char *const WANT[] = {
        "Success", "End of file", "No such file", "Permission denied",
        "Failure", "Bad message", "No connection", "Connection lost",
        "Operation unsupported",
    };
    bool all = true;

    for (uint32_t i = 0; i < AXL_ARRAY_SIZE(WANT); i++) {
        if (axl_strcmp(sftp_status_message(i), WANT[i]) != 0) {
            all = false;
        }
    }
    test_check(all, "status: sftp-server's text for codes 0-8");
    test_check(axl_strcmp(sftp_status_message(9), "Unknown error") == 0,
               "status: any other code is \"Unknown error\"");
}

static void
test_attrs_synth(void)
{
    SftpAttrs a;

    sftp_attrs_synth(&a, false, false, 7, 100);
    test_check(a.flags == (SFTP_ATTR_SIZE | SFTP_ATTR_PERMISSIONS | SFTP_ATTR_ACMODTIME)
                   && a.perm == 0100644 && a.size == 7 && a.atime == 100 && a.mtime == 100,
               "synth: a file is 0644 with its size and time");
    sftp_attrs_synth(&a, false, true, 7, 100);
    test_check(a.perm == 0100444, "synth: a read-only file loses its write bits");
    sftp_attrs_synth(&a, true, false, 99, 100);
    test_check(a.perm == 040755 && a.size == 0, "synth: a directory is 0755, size 0");
    sftp_attrs_synth(&a, true, true, 0, 100);
    test_check(a.perm == 040555, "synth: a read-only directory is 0555");
    sftp_attrs_synth(&a, false, false, 0, 0x100000000ull);
    test_check(a.mtime == 0xffffffffu, "synth: a time past 32 bits is clamped");
}

static void
test_longname(void)
{
    SftpAttrs a;
    char      l[160];

    sftp_attrs_synth(&a, false, false, 1234, 1700000000u);
    sftp_longname(l, sizeof(l), "name", &a, 1700000100u);
    test_check(axl_strcmp(l, "-rw-r--r--    1 0        0            1234 Nov 14 22:13 name") == 0,
               "longname: a recent file shows the time of day");
    sftp_attrs_synth(&a, true, false, 0, 1577836800u);
    sftp_longname(l, sizeof(l), "dir", &a, 1700000000u);
    test_check(axl_strcmp(l, "drwxr-xr-x    1 0        0               0 Jan  1  2020 dir") == 0,
               "longname: an old directory shows the year");
    sftp_attrs_synth(&a, false, true, 0, 1700000000u - 15768000u);
    sftp_longname(l, sizeof(l), "x", &a, 1700000000u);
    test_check(axl_strcmp(l, "-r--r--r--    1 0        0               0 May 16  2023 x") == 0,
               "longname: exactly half a year old shows the year (read-only)");
    sftp_attrs_synth(&a, false, false, 0, 1700000000u - 15767999u);
    sftp_longname(l, sizeof(l), "x", &a, 1700000000u);
    test_check(axl_strcmp(l, "-rw-r--r--    1 0        0               0 May 16 10:13 x") == 0,
               "longname: one second younger shows the time");
    sftp_attrs_synth(&a, false, false, 123456789012ull, 1700000001u);
    sftp_longname(l, sizeof(l), "f", &a, 1700000000u);
    test_check(axl_strcmp(l, "-rw-r--r--    1 0        0        123456789012 Nov 14  2023 f") == 0,
               "longname: a future time shows the year; a wide size widens");
    sftp_attrs_synth(&a, false, false, 0, 951782400u);
    sftp_longname(l, sizeof(l), "leap", &a, 951782400u);
    test_check(axl_strcmp(l, "-rw-r--r--    1 0        0               0 Feb 29 00:00 leap") == 0,
               "longname: a leap day");
    sftp_longname(l, 12, "leap", &a, 951782400u);
    test_check(axl_strcmp(l, "-rw-r--r-- ") == 0, "longname: truncated to its room");
}

static void
test_handles(void)
{
    uint8_t  h[SFTP_HANDLE_LEN];
    uint32_t idx = 0;
    uint32_t gen = 0;

    sftp_handle_encode(h, 15, 0xfedcba98u);
    test_check(axl_strcmp(hex(h, sizeof(h)), "0000000ffedcba98") == 0,
               "handle: index then generation, big-endian");
    test_check(sftp_handle_decode((SftpStr){ h, 8 }, &idx, &gen) && idx == 15
                   && gen == 0xfedcba98u,
               "handle: decodes back");
    test_check(!sftp_handle_decode((SftpStr){ h, 7 }, &idx, &gen)
                   && !sftp_handle_decode((SftpStr){ h, 9 }, &idx, &gen),
               "handle: any other length is refused");
}

/* ---- paths ---------------------------------------------------------------- */

static void
path_is(const char *in, const char *canon, const char *fs, const char *vol)
{
    SftpPath p;
    char     name[200];
    int      rc = sftp_path_normalize((const uint8_t *)in, axl_strlen(in), &p);

    axl_snprintf(name, sizeof(name), "path: \"%s\" -> %s | %s | %s", in, canon, fs, vol);
    test_check(rc == AXL_OK && axl_strcmp(p.canon, canon) == 0
                   && axl_strcmp(p.fs, fs) == 0 && axl_strcmp(p.volume, vol) == 0,
               name);
}

static void
path_bad(const uint8_t *in, size_t len, const char *what)
{
    SftpPath p;
    char     name[120];

    axl_snprintf(name, sizeof(name), "path refused: %s", what);
    test_check(sftp_path_normalize(in, len, &p) == AXL_INVALID, name);
}

static void
test_paths(void)
{
    path_is("/", "/", "", "");
    path_is("", "/", "", "");
    path_is(".", "/", "", "");
    path_is("..", "/", "", "");
    path_is("/..", "/", "", "");
    path_is("/../..", "/", "", "");
    path_is("/fs0", "/fs0", "fs0:\\", "fs0");
    path_is("/fs0/", "/fs0", "fs0:\\", "fs0");
    path_is("/FS0/a/b", "/fs0/a/b", "fs0:\\a\\b", "fs0");
    path_is("/fs0/a/../b/./c//", "/fs0/b/c", "fs0:\\b\\c", "fs0");
    path_is("/fs0/..", "/", "", "");
    path_is("/fs0/../../../fs1/x", "/fs1/x", "fs1:\\x", "fs1");
    path_is("fs0:/a/b", "/fs0/a/b", "fs0:\\a\\b", "fs0");
    path_is("FS0:\\a\\b", "/fs0/a/b", "fs0:\\a\\b", "fs0");
    path_is("fs0:", "/fs0", "fs0:\\", "fs0");
    path_is("fs0:\\", "/fs0", "fs0:\\", "fs0");
    path_is("fs0:a", "/fs0/a", "fs0:\\a", "fs0");
    path_is("fs0:a:b", "/fs0/a:b", "fs0:\\a:b", "fs0");
    path_is("fs0:\\..\\..", "/", "", "");
    path_is("/fs0:/a", "/fs0/a", "fs0:\\a", "fs0");
    path_is("a/b", "/a/b", "a:\\b", "a");
    path_is("./fs2/x", "/fs2/x", "fs2:\\x", "fs2");
    path_is("/fs0/a\\b", "/fs0/a/b", "fs0:\\a\\b", "fs0");
    path_is("/fs0/Dir Name/File.TXT", "/fs0/Dir Name/File.TXT",
            "fs0:\\Dir Name\\File.TXT", "fs0");
    path_is("/RD/x", "/rd/x", "rd:\\x", "rd");
    path_is("/fs0/caf\xc3\xa9", "/fs0/caf\xc3\xa9", "fs0:\\caf\xc3\xa9", "fs0");
    path_is("/fs0/...", "/fs0/...", "fs0:\\...", "fs0");

    path_bad((const uint8_t *)"/fs0/a\0b", 8, "a NUL");
    path_bad((const uint8_t *)"/fs0/\x01", 6, "a control byte");
    path_bad((const uint8_t *)"/fs0/\x1f", 6, "0x1f");
    path_bad((const uint8_t *)"/fs0/\x7f", 6, "DEL");
    path_bad((const uint8_t *)"/fs0/a\r\n", 8, "CR LF");
    path_bad((const uint8_t *)":", 1, "an empty volume before ':'");
    path_bad((const uint8_t *)":/x", 3, "an empty volume before ':/'");

    char vol[64];
    axl_memset(vol, 'v', sizeof(vol));
    vol[0] = '/';
    path_bad((const uint8_t *)vol, 1 + SFTP_VOLUME_MAX, "a volume of SFTP_VOLUME_MAX bytes");
    SftpPath p;
    test_check(sftp_path_normalize((const uint8_t *)vol, SFTP_VOLUME_MAX, &p) == AXL_OK
                   && axl_strlen(p.volume) == SFTP_VOLUME_MAX - 1u,
               "path: a volume of SFTP_VOLUME_MAX - 1 bytes is accepted");

    /* "/fs0/x..." and "fs0:\x..." are the same length: SFTP_PATH_MAX - 1
       bytes fit both forms, one more does not. */
    static uint8_t lp[SFTP_PATH_MAX + 8];
    lp[0] = '/'; lp[1] = 'f'; lp[2] = 's'; lp[3] = '0'; lp[4] = '/';
    axl_memset(lp + 5, 'x', sizeof(lp) - 5);
    test_check(sftp_path_normalize(lp, SFTP_PATH_MAX - 1u, &p) == AXL_OK
                   && axl_strlen(p.canon) == SFTP_PATH_MAX - 1u
                   && axl_strlen(p.fs) == SFTP_PATH_MAX - 1u,
               "path: the longest that fits");
    test_check(sftp_path_normalize(lp, SFTP_PATH_MAX, &p) == AXL_NO_RESOURCES,
               "path: one byte longer than fits is AXL_NO_RESOURCES (does not fit)");
    /* The Shell form GROWS: "fs0:" + 1019 bytes is 1023 in, but "/fs0/"
       + 1019 is 1024 in the canonical form -- does not fit either. */
    static uint8_t sp[SFTP_PATH_MAX];
    sp[0] = 'f'; sp[1] = 's'; sp[2] = '0'; sp[3] = ':';
    axl_memset(sp + 4, 'x', sizeof(sp) - 4);
    test_check(sftp_path_normalize(sp, SFTP_PATH_MAX - 1u, &p) == AXL_NO_RESOURCES,
               "path: a 1023-byte Shell-form path that grows past the room is AXL_NO_RESOURCES");
    test_check(sftp_path_normalize(sp, SFTP_PATH_MAX - 2u, &p) == AXL_OK
                   && axl_strlen(p.canon) == SFTP_PATH_MAX - 1u,
               "path: one byte shorter, the Shell form fits exactly");
}

/* ---- the session handler (sshd-sftp.c) -------------------------------------
   Sessions driven through capture callbacks: every reply byte is compared
   exactly, in hex. Expected statuses are the host's OpenSSH 9.9p1
   sftp-server's answers to the same packets, captured by driving it on its
   stdin (its process_* status paths). */

/* What a session wrote and how it ended. */
typedef struct {
    uint8_t  buf[48u * 1024u];
    size_t   len;
    unsigned writes;     /* write calls accepted */
    unsigned busy;       /* refuse this many more writes with AXL_BUSY */
    size_t   consumed;   /* bytes the session reported done with */
    bool     ended;
    uint32_t status;
} Cap;

static int
cap_write(void *ctx, const void *buf, size_t len)
{
    Cap *c = ctx;

    if (c->busy > 0) {
        c->busy--;
        return AXL_BUSY;
    }
    if (len > sizeof(c->buf) - c->len) {
        return AXL_ERR;
    }
    axl_memcpy(c->buf + c->len, buf, len);
    c->len += len;
    c->writes++;
    return AXL_OK;
}

static void
cap_consumed(void *ctx, size_t n)
{
    Cap *c = ctx;

    c->consumed += n;
}

static void
cap_end(void *ctx, uint32_t status)
{
    Cap *c = ctx;

    c->ended  = true;
    c->status = status;
}

/* Hex of everything captured since @a from, into a heap string. */
static char *
cap_hex(const Cap *c, size_t from)
{
    static const char D[] = "0123456789abcdef";
    size_t n   = c->len - from;
    char  *out = axl_malloc(2 * n + 1);

    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = D[c->buf[from + i] >> 4];
        out[2 * i + 1] = D[c->buf[from + i] & 15];
    }
    out[2 * n] = '\0';
    return out;
}

/* Feed the hex @a h to @a s, then compare everything written since then
   with @a want (hex). */
static bool
sess_says(SftpSession *s, Cap *c, const char *h, const char *want)
{
    static uint8_t b[4096];
    size_t from = c->len;
    size_t n    = unhex(h, b, sizeof(b));

    sftp_session_data(s, b, n);
    char *got = cap_hex(c, from);
    bool  ok  = got != NULL && axl_strcmp(got, want) == 0;
    if (!ok) {
        axl_printf("    got:  [%s]\n    want: [%s]\n", got != NULL ? got : "(oom)", want);
    }
    axl_free(got);
    return ok;
}

/* A packet: its length, then the body in hex. */
static const char *
pk(const char *body)
{
    static char out[4][6200];
    static unsigned k;
    char *o = out[k++ % 4];

    axl_snprintf(o, sizeof(out[0]), "%08x%s", (unsigned)(axl_strlen(body) / 2), body);
    return o;
}

#define S_INIT      "0100000003"
/* VERSION 3 advertising posix-rename@openssh.com and limits@openssh.com,
   each with data "1", in sftp-server's order; read-only mode leaves out
   posix-rename, a write, as sftp-server -R leaves out its writes. */
#define R_VERSION   "00000041" "02" "00000003" \
                    "00000018" "706f7369782d72656e616d65406f70656e7373682e636f6d" \
                    "00000001" "31" \
                    "00000012" "6c696d697473406f70656e7373682e636f6d" "00000001" "31"
#define R_VERSION_RO "00000020" "02" "00000003" \
                     "00000012" "6c696d697473406f70656e7373682e636f6d" "00000001" "31"
/* "/fs0/sftp-unit.txt" */
#define S_FIXTURE   "000000122f6673302f736674702d756e69742e747874"
#define FIXTURE_FS  "fs0:\\sftp-unit.txt"

/* STATUS for id @a id with @a code, sftp-server's text, as hex. */
static const char *
st(uint32_t id, uint32_t code)
{
    static char out[4][256];
    static unsigned k;
    char       *o   = out[k++ % 4];
    const char *msg = sftp_status_message(code);
    size_t      ml  = axl_strlen(msg);
    size_t      n   = 0;

    n += (size_t)axl_snprintf(o + n, sizeof(out[0]) - n, "%08x65%08x%08x%08x",
                              (unsigned)(1 + 4 + 4 + 4 + ml + 4), (unsigned)id,
                              (unsigned)code, (unsigned)ml);
    for (size_t i = 0; i < ml; i++) {
        n += (size_t)axl_snprintf(o + n, sizeof(out[0]) - n, "%02x",
                                  (unsigned)(unsigned char)msg[i]);
    }
    axl_snprintf(o + n, sizeof(out[0]) - n, "00000000");
    return o;
}

/* HANDLE for id @a id naming slot @a idx, generation @a gen, as hex. */
static const char *
hd(uint32_t id, uint32_t idx, uint32_t gen)
{
    static char out[4][64];
    static unsigned k;
    char *o = out[k++ % 4];

    axl_snprintf(o, sizeof(out[0]), "0000001166%08x00000008%08x%08x",
                 (unsigned)id, (unsigned)idx, (unsigned)gen);
    return o;
}

/* An open session over a fresh capture, INIT already answered. */
static SftpSession *
sess_new(Cap *c, uint32_t flags)
{
    axl_memset(c, 0, sizeof(*c));
    SftpSession *s = sftp_session_open(cap_write, cap_end, cap_consumed, c, flags,
                                       8192);
    const char  *want = (flags & SFTP_SESSION_READ_ONLY) != 0 ? R_VERSION_RO : R_VERSION;
    if (s != NULL && !sess_says(s, c, pk(S_INIT), want)) {
        sftp_session_close(s);
        return NULL;
    }
    return s;
}

/* A request built from @a fmt (hex, printf-style), framed, and the reply it
   must get (hex). */
static bool
says(SftpSession *s, Cap *c, const char *want, const char *fmt, ...)
{
    static char body[6000];
    va_list     ap;

    va_start(ap, fmt);
    axl_vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    return sess_says(s, c, pk(body), want);
}

/* An SFTP string -- length, then bytes -- of @a str, as hex. */
static const char *
hs(const char *str)
{
    static char out[8][700];
    static unsigned k;
    char  *o = out[k++ % 8];
    size_t n = axl_strlen(str);
    size_t w = (size_t)axl_snprintf(o, sizeof(out[0]), "%08x", (unsigned)n);

    for (size_t i = 0; i < n && w + 3 < sizeof(out[0]); i++) {
        w += (size_t)axl_snprintf(o + w, sizeof(out[0]) - w, "%02x",
                                  (unsigned)(unsigned char)str[i]);
    }
    return o;
}

/* The handle string for slot @a idx, generation @a gen, as hex. */
static const char *
hh(uint32_t idx, uint32_t gen)
{
    static char out[4][32];
    static unsigned k;
    char *o = out[k++ % 4];

    axl_snprintf(o, sizeof(out[0]), "00000008%08x%08x", (unsigned)idx, (unsigned)gen);
    return o;
}

/* DATA for id @a id carrying @a str, as hex. */
static const char *
dt(uint32_t id, const char *str)
{
    static char out[2][600];
    static unsigned k;
    char  *o = out[k++ % 2];
    size_t n = axl_strlen(str);

    axl_snprintf(o, sizeof(out[0]), "%08x67%08x%s", (unsigned)(1 + 4 + 4 + n),
                 (unsigned)id, hs(str));
    return o;
}

/* Whether the file at @a fs holds exactly @a len bytes of @a want. */
static bool
file_is(const char *fs, const void *want, size_t len)
{
    void  *buf = NULL;
    size_t n   = 0;

    if (axl_file_get_contents(fs, &buf, &n) != AXL_OK) {
        return false;
    }
    bool ok = n == len && (len == 0 || axl_memcmp(buf, want, len) == 0);
    if (!ok) {
        axl_printf("    %s holds %u bytes\n", fs, (unsigned)n);
    }
    axl_free(buf);
    return ok;
}

static bool
exists(const char *fs)
{
    AxlFsEntry e;

    return axl_file_info(fs, &e) == AXL_OK;
}

static bool
is_dir(const char *fs)
{
    AxlFsEntry e;

    return axl_file_info(fs, &e) == AXL_OK && axl_fs_entry_is_dir(&e);
}

/* The modification time a path holds, or 0. */
static uint64_t
mtime_of(const char *fs)
{
    AxlFsEntry e;

    return axl_file_info(fs, &e) == AXL_OK ? e.mtime_unix : 0;
}

/* The write tests' directory, both forms. */
#define W_FS     "fs0:\\sftp-w"
#define W        "/fs0/sftp-w"
/* A file test-axl.sh stages with the FAT read-only attribute. */
#define RO_FS    "fs0:\\sftp-ro.txt"

/* Remove @a fs and everything under it; true when it is gone (or was never
   there). */
static bool
rm_tree(const char *fs)
{
    AxlFsEntry e;

    if (axl_file_info(fs, &e) != AXL_OK) {
        return true;
    }
    if (!axl_fs_entry_is_dir(&e)) {
        return axl_file_delete(fs) == AXL_OK;
    }
    AxlDir *d  = axl_dir_open(fs);
    bool    ok = d != NULL;
    while (ok) {
        e.struct_size = sizeof(e);
        e.version     = AXL_FS_ENTRY_VERSION;
        if (!axl_dir_read(d, &e)) {
            break;
        }
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        /* The path whole, never cut: a cut one names another entry, or none. */
        char *child = axl_asprintf("%s\\%s", fs, e.name);
        ok = child != NULL && rm_tree(child);
        axl_free(child);
    }
    axl_dir_close(d);
    return ok && axl_dir_rmdir(fs) == AXL_OK;
}

/* Start the write tests' directory from nothing. */
static bool
w_reset(void)
{
    return rm_tree(W_FS);
}

/* OPEN of @a path with @a pflags and no attributes, id @a id, as hex. */
static const char *
op_open_hex(uint32_t id, const char *path, uint32_t pflags)
{
    static char out[2][800];
    static unsigned k;
    char *o = out[k++ % 2];

    axl_snprintf(o, sizeof(out[0]), "03%08x%s%08x00000000", (unsigned)id, hs(path),
                 (unsigned)pflags);
    return o;
}

static void
test_session_basics(void)
{
    static Cap c;
    char       h[512];

    test_check(axl_file_set_contents(FIXTURE_FS, "hello\n", 6) == AXL_OK,
               "session: the fixture file is written");
    SftpSession *s = sess_new(&c, 0);
    test_check(s != NULL && sftp_sessions_open() == 1,
               "session: opens and answers INIT with VERSION 3, posix-rename and limits");
    if (s == NULL) {
        test_skip_n(16, "no session");
        return;
    }
    test_check(sess_says(s, &c, pk(S_INIT), R_VERSION),
               "session: a second INIT is answered again, as sftp-server does");
    /* OPEN for read -> handle (slot 0, generation 1); READ 0 bytes -> an
       empty DATA; READ 100 at 0 -> the 6 bytes; at 6 -> EOF. */
    axl_snprintf(h, sizeof(h), "03" "00000002" S_FIXTURE "00000001" "00000000");
    test_check(sess_says(s, &c, pk(h), hd(2, 0, 1)),
               "session: OPEN for read gives handle slot 0, generation 1");
    test_check(sess_says(s, &c, pk("05" "00000003" "000000080000000000000001"
                                   "0000000000000000" "00000000"),
                         "000000096700000003" "00000000"),
               "session: READ of 0 bytes is an empty DATA, as sftp-server sends");
    test_check(sess_says(s, &c, pk("05" "00000004" "000000080000000000000001"
                                   "0000000000000000" "00000064"),
                         "0000000f6700000004" "00000006" "68656c6c6f0a"),
               "session: READ returns the file's 6 bytes");
    test_check(sess_says(s, &c, pk("05" "00000005" "000000080000000000000001"
                                   "0000000000000006" "00000064"),
                         st(5, SFTP_FX_EOF)),
               "session: READ at the end is EOF");
    /* Past the end the firmware's Read() fails (EFI_DEVICE_ERROR); read(2)
       there returns 0, which sftp-server answers EOF. */
    test_check(sess_says(s, &c, pk("05" "00000055" "000000080000000000000001"
                                   "0000000000000064" "00000064"),
                         st(0x55, SFTP_FX_EOF))
                   && sess_says(s, &c, pk("05" "00000056" "000000080000000000000001"
                                          "0000000000000000" "00000064"),
                                "0000000f6700000056" "00000006" "68656c6c6f0a"),
               "session: READ past the end is EOF too, and the handle reads on");
    test_check(sess_says(s, &c, pk("05" "00000006" "000000080000000000000001"
                                   "8000000000000000" "0000000a"),
                         st(6, SFTP_FX_BAD_MESSAGE)),
               "session: READ at an offset over INT64_MAX is BAD_MESSAGE (sftp-server's EINVAL)");
    /* CLOSE, then the stale handle: CLOSE -> NO_SUCH_FILE, READ -> FAILURE,
       as sftp-server answers an unknown handle. */
    test_check(sess_says(s, &c, pk("04" "00000007" "000000080000000000000001"),
                         st(7, SFTP_FX_OK)),
               "session: CLOSE is OK");
    test_check(sess_says(s, &c, pk("04" "00000008" "000000080000000000000001"),
                         st(8, SFTP_FX_NO_SUCH_FILE)),
               "session: CLOSE of the closed (stale) handle is NO_SUCH_FILE");
    test_check(sess_says(s, &c, pk("05" "00000009" "000000080000000000000001"
                                   "0000000000000000" "00000064"),
                         st(9, SFTP_FX_FAILURE)),
               "session: READ of the stale handle is FAILURE");
    axl_snprintf(h, sizeof(h), "03" "0000000a" S_FIXTURE "00000001" "00000000");
    test_check(sess_says(s, &c, pk(h), hd(10, 0, 2)),
               "session: slot 0 reused gets generation 2");
    test_check(sess_says(s, &c, pk("05" "0000000b" "000000080000000000000001"
                                   "0000000000000000" "00000064"),
                         st(11, SFTP_FX_FAILURE)),
               "session: the old generation of a reused slot is FAILURE");
    test_check(sess_says(s, &c, pk("05" "0000000c" "000000080000000500000002"
                                   "0000000000000000" "00000064"),
                         st(12, SFTP_FX_FAILURE)),
               "session: a forged handle (an unused slot) is FAILURE");
    test_check(sess_says(s, &c, pk("05" "0000000d" "00000004" "00000000"
                                   "0000000000000000" "00000064"),
                         st(13, SFTP_FX_FAILURE)),
               "session: a handle of the wrong length is FAILURE");
    /* OPENDIR on a file: sftp-server's ENOTDIR is NO_SUCH_FILE. */
    axl_snprintf(h, sizeof(h), "0b" "0000000e" S_FIXTURE);
    test_check(sess_says(s, &c, pk(h), st(14, SFTP_FX_NO_SUCH_FILE)),
               "session: OPENDIR of a file is NO_SUCH_FILE");
    test_check(!c.ended, "session: none of that ended the session");
    sftp_session_close(s);
    test_check(sftp_sessions_open() == 0, "session: closing it counts it out");
}

/* Read-only mode (AxlSftpReadOnly): every write op, and OPEN with each write
   flag, is PERMISSION_DENIED -- even one that would fail anyway (a REMOVE of
   nothing), as sftp-server -R refuses before it looks -- and leaves the
   volume as it was. Reads still work. sftp-server -R also admits READ with
   APPEND or EXCL, which cannot change a byte; here any write flag is a
   write, so OPEN is classed by its flags alone. */
static void
test_session_read_only(void)
{
    static Cap c;
    char       h[512];
    bool       all = true;

    /* What an interrupted earlier run (or one under a sabotage) can leave:
       the write tests' directory with entries in it. The fixture starts
       from nothing whatever it finds. */
    test_check(axl_dir_mkdir(W_FS) == AXL_OK && axl_dir_mkdir(W_FS "\\newdir") == AXL_OK
                   && axl_file_set_contents(W_FS "\\newdir\\x.txt", "x", 1) == AXL_OK
                   && axl_file_set_contents(W_FS "\\created.txt", "x", 1) == AXL_OK,
               "read-only: a previous run's leftovers are planted");
    test_check(w_reset() && !exists(W_FS), "read-only: the leftovers are swept");
    test_check(axl_dir_mkdir(W_FS) == AXL_OK
                   && axl_file_set_contents(W_FS "\\ro.txt", "keep", 4) == AXL_OK
                   && axl_file_set_mtime(W_FS "\\ro.txt", 1000000000u) == AXL_OK,
               "read-only: the fixture is written, its time 1000000000");
    SftpSession *s = sess_new(&c, SFTP_SESSION_READ_ONLY);
    if (s == NULL) {
        test_skip_n(8, "no session");
        return;
    }
    /* A READ handle first (slot 0, generation 1): WRITE and FSETSTAT on it
       are refused by the mode, not by the handle. */
    test_check(says(s, &c, hd(99, 0, 1), "%s", op_open_hex(99, W "/ro.txt", SFTP_OPEN_READ)),
               "read-only: OPEN for read gives a handle");
    static const char *const OPS[] = {
        "06" "%08x" "000000080000000000000001" "0000000000000000" "00000001" "78",
        "09" "%08x" "%s" "00000001" "0000000000000000",
        "0a" "%08x" "000000080000000000000001" "00000001" "0000000000000000",
        "0d" "%08x" "%s",
        "0e" "%08x" "%s" "00000000",
        "0f" "%08x" "%s",
        "12" "%08x" "%s" "%s",
        "14" "%08x" "%s" "%s",
        /* A time alone, by path and by handle (mtime 1000000004). */
        "09" "%08x" "%s" "00000008" "00000005" "3b9aca04",
        "0a" "%08x" "000000080000000000000001" "00000008" "00000005" "3b9aca04",
        /* posix-rename@openssh.com: a write by its extension's name. */
        "c8" "%08x" "00000018" "706f7369782d72656e616d65406f70656e7373682e636f6d" "%s" "%s",
    };
    /* The path each op names (a hex SFTP string), or "" for a handle op. */
    const char *const PATHS[] = {
        "", "ro.txt", "", "ro.txt", "newdir", W, "ro.txt", "ln", "ro.txt", "", "ro.txt",
    };
    uint32_t id = 100;
    for (size_t i = 0; i < AXL_ARRAY_SIZE(OPS); i++, id++) {
        char p[64];
        axl_snprintf(p, sizeof(p), W "/%s", PATHS[i]);
        const char *ph = PATHS[i][0] == '\0' ? "" : (PATHS[i][0] == '/' ? hs(PATHS[i]) : hs(p));
        axl_snprintf(h, sizeof(h), OPS[i], (unsigned)id, ph, hs(W "/moved.txt"));
        all = sess_says(s, &c, pk(h), st(id, SFTP_FX_PERMISSION_DENIED)) && all;
    }
    test_check(all, "read-only: WRITE, SETSTAT, FSETSTAT (a time alone too), REMOVE, MKDIR, "
                    "RMDIR, RENAME, SYMLINK and posix-rename are PERMISSION_DENIED");
    all = true;
    static const uint32_t FLAGS[] = {
        SFTP_OPEN_WRITE, SFTP_OPEN_APPEND, SFTP_OPEN_CREAT, SFTP_OPEN_TRUNC,
        SFTP_OPEN_EXCL, SFTP_OPEN_READ | SFTP_OPEN_WRITE, SFTP_OPEN_READ | SFTP_OPEN_APPEND,
        SFTP_OPEN_READ | SFTP_OPEN_EXCL, SFTP_OPEN_READ | SFTP_OPEN_TRUNC,
        SFTP_OPEN_WRITE | SFTP_OPEN_CREAT | SFTP_OPEN_TRUNC,
    };
    for (size_t i = 0; i < AXL_ARRAY_SIZE(FLAGS); i++, id++) {
        all = says(s, &c, st(id, SFTP_FX_PERMISSION_DENIED), "%s",
                   op_open_hex(id, W "/ro.txt", FLAGS[i])) && all;
    }
    all = says(s, &c, st(id, SFTP_FX_PERMISSION_DENIED), "%s",
               op_open_hex(id, W "/created.txt", SFTP_OPEN_WRITE | SFTP_OPEN_CREAT)) && all;
    id++;
    test_check(all, "read-only: OPEN with each write flag, alone or with READ, is "
                    "PERMISSION_DENIED");
    test_check(file_is(W_FS "\\ro.txt", "keep", 4) && !exists(W_FS "\\created.txt")
                   && !exists(W_FS "\\newdir") && !exists(W_FS "\\moved.txt")
                   && is_dir(W_FS) && mtime_of(W_FS "\\ro.txt") == 1000000000u,
               "read-only: nothing on the volume changed, the time included");
    /* Reads still work: a READ on the handle, a second OPEN for read (an
       unknown flag bit is not a write flag), STAT. */
    test_check(says(s, &c, dt(id, "keep"),
                    "05%08x" "000000080000000000000001" "0000000000000000" "00000064",
                    (unsigned)id),
               "read-only: READ still reads");
    id++;
    test_check(says(s, &c, hd(id, 1, 1), "%s",
                    op_open_hex(id, W "/ro.txt", SFTP_OPEN_READ | 0x40u)),
               "read-only: OPEN for read with an unknown flag bit is a read");
    id++;
    char want[96];
    axl_snprintf(want, sizeof(want), "0000001d69%08x0000000d0000000000000000000041ed"
                 "0000000000000000", (unsigned)id);
    test_check(says(s, &c, want, "11%08x%s", (unsigned)id, hs("/")),
               "read-only: STAT still answers");
    test_check(!c.ended, "read-only: refusing writes ended nothing");
    sftp_session_close(s);
}

/* The dispatcher's table classes every request type the codec decodes, and
   none it does not: a type added to sftp_decode() without a class fails
   here. A known type is one whose bare header (type, id) does not decode,
   since every request but INIT carries a field after its id. */
static void
test_request_classes(void)
{
    bool all = true;

    for (unsigned t = 0; t < 256; t++) {
        uint8_t     b[5] = { (uint8_t)t, 0, 0, 0, 1 };
        SftpRequest r;
        bool        known = t == SFTP_INIT || sftp_decode(b, sizeof(b), &r) != AXL_OK;
        bool        classed = sftp_request_class((uint8_t)t) != SFTP_REQ_UNKNOWN;
        if (known != classed) {
            axl_printf("    type %u: decoded %s, classed %s\n", t, known ? "yes" : "no",
                       classed ? "yes" : "no");
            all = false;
        }
    }
    test_check(all, "classes: exactly the request types the codec decodes are classed");
    static const uint8_t WRITES[] = {
        SFTP_WRITE, SFTP_SETSTAT, SFTP_FSETSTAT, SFTP_REMOVE, SFTP_MKDIR, SFTP_RMDIR,
        SFTP_RENAME, SFTP_SYMLINK,
    };
    static const uint8_t READS[] = {
        SFTP_INIT, SFTP_CLOSE, SFTP_READ, SFTP_LSTAT, SFTP_FSTAT, SFTP_OPENDIR,
        SFTP_READDIR, SFTP_REALPATH, SFTP_STAT, SFTP_READLINK,
    };
    all = sftp_request_class(SFTP_OPEN) == SFTP_REQ_OPEN
          && sftp_request_class(SFTP_EXTENDED) == SFTP_REQ_EXTENDED;
    for (size_t i = 0; i < sizeof(WRITES); i++) {
        all = sftp_request_class(WRITES[i]) == SFTP_REQ_WRITES && all;
    }
    for (size_t i = 0; i < sizeof(READS); i++) {
        all = sftp_request_class(READS[i]) == SFTP_REQ_READS && all;
    }
    test_check(all, "classes: the writes are sftp-server's does_write set; OPEN goes by "
                    "its flags, EXTENDED by its extension's name");
}

/* EXTENDED is classed by name, from a table every served extension must be
   in: each row resolves to itself with a read or write class, and a name
   the table lacks is unknown -- answered OP_UNSUPPORTED whether or not the
   session is read-only, as sftp-server's process_extended() answers an
   extension it does not serve. */
static void
test_extensions(void)
{
    static Cap  c;
    bool        all = true;
    const char *name;
    static const char NOPE[] = "no-such-extension@openssh.com";

    for (size_t i = 0; (name = sftp_extension_name(i)) != NULL; i++) {
        SftpRequestClass cls = sftp_extension_class((const uint8_t *)name,
                                                    axl_strlen(name));
        if (cls != SFTP_REQ_READS && cls != SFTP_REQ_WRITES) {
            axl_printf("    extension %s: class %d\n", name, (int)cls);
            all = false;
        }
    }
    test_check(all, "extensions: every served extension resolves to a read or write class");
    /* Non-vacuity: the loop above proves nothing over an empty table, and
       read-only mode's EXTENDED branch is only reachable through a row. */
    static const char PR[] = "posix-rename@openssh.com";
    static const char LIM[] = "limits@openssh.com";
    test_check(sftp_extension_name(0) != NULL
                   && axl_strcmp(sftp_extension_name(0), PR) == 0
                   && sftp_extension_name(1) != NULL
                   && axl_strcmp(sftp_extension_name(1), LIM) == 0
                   && sftp_extension_name(2) == NULL,
               "extensions: the table is exactly posix-rename, then limits");
    test_check(sftp_extension_class((const uint8_t *)PR, sizeof(PR) - 1u) == SFTP_REQ_WRITES
                   && sftp_extension_class((const uint8_t *)LIM, sizeof(LIM) - 1u)
                          == SFTP_REQ_READS,
               "extensions: posix-rename is a write, limits a read (sftp-server's does_write)");
    /* A prefix or an extension of a served name is not that name. */
    test_check(sftp_extension_class((const uint8_t *)PR, sizeof(PR) - 2u) == SFTP_REQ_UNKNOWN
                   && sftp_extension_class((const uint8_t *)"limits@openssh.comx", 19)
                          == SFTP_REQ_UNKNOWN,
               "extensions: a name is matched whole, not by prefix");
    for (uint32_t ro = 0; ro < 2; ro++) {
        axl_memset(&c, 0, sizeof(c));
        SftpSession *s = sftp_session_open(cap_write, cap_end, cap_consumed, &c,
                                           ro != 0 ? SFTP_SESSION_READ_ONLY : 0, 8192);
        test_check(s != NULL && sess_says(s, &c, pk(S_INIT), ro != 0 ? R_VERSION_RO : R_VERSION),
                   ro != 0 ? "extensions: read-only VERSION advertises limits alone, as "
                             "sftp-server -R leaves its writes out"
                           : "extensions: VERSION advertises posix-rename and limits, "
                             "each \"1\"");
        test_check(s != NULL
                       && says(s, &c, "00000025c9" "00000009" "0000000000040000"
                               "0000000000008000" "000000000003fc00" "0000000000000010",
                               "c8%08x%s", 9u, hs(LIM)),
                   ro != 0 ? "extensions: limits is answered in read-only mode too"
                           : "extensions: limits: 256 KB packet, 32 KB read, 256 KB - 1024 "
                             "write, 16 handles");
        sftp_session_close(s);
    }
    test_check(sftp_extension_class((const uint8_t *)NOPE, sizeof(NOPE) - 1u)
                   == SFTP_REQ_UNKNOWN
                   && sftp_extension_class((const uint8_t *)"", 0) == SFTP_REQ_UNKNOWN,
               "extensions: a name the table lacks is unknown");
    for (uint32_t ro = 0; ro < 2; ro++) {
        SftpSession *s = sess_new(&c, ro != 0 ? SFTP_SESSION_READ_ONLY : 0);
        test_check(s != NULL
                       && says(s, &c, st(7, SFTP_FX_OP_UNSUPPORTED), "c8%08x%s", 7u, hs(NOPE))
                       && !c.ended,
                   ro != 0 ? "extensions: an unknown one is OP_UNSUPPORTED in read-only mode"
                           : "extensions: an unknown one is OP_UNSUPPORTED");
        sftp_session_close(s);
    }
}

/* OPEN for writing, WRITE, CLOSE: statuses as sftp-server's for the same
   condition (a missing file is NO_SUCH_FILE, an existing one under EXCL or a
   directory is FAILURE, a READ on a write-only handle and a WRITE on a
   read-only one are NO_SUCH_FILE -- its EBADF), and the bytes on the
   volume exact. */
static void
test_session_open_write(void)
{
    static Cap c;

    axl_file_delete(W_FS "\\new.txt");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(18, "no session");
        return;
    }
    const uint32_t WCT = SFTP_OPEN_WRITE | SFTP_OPEN_CREAT | SFTP_OPEN_TRUNC;
    test_check(says(s, &c, st(1, SFTP_FX_NO_SUCH_FILE), "%s",
                    op_open_hex(1, W "/new.txt", SFTP_OPEN_WRITE))
                   && !exists(W_FS "\\new.txt"),
               "open-write: WRITE without CREAT on a missing file is NO_SUCH_FILE");
    test_check(says(s, &c, st(2, SFTP_FX_NO_SUCH_FILE), "%s",
                    op_open_hex(2, W "/nodir/x", SFTP_OPEN_WRITE | SFTP_OPEN_CREAT)),
               "open-write: CREAT in a missing directory is NO_SUCH_FILE");
    test_check(says(s, &c, hd(3, 0, 1), "%s", op_open_hex(3, W "/new.txt", WCT))
                   && file_is(W_FS "\\new.txt", "", 0),
               "open-write: WRITE|CREAT|TRUNC creates the file and gives a handle");
    test_check(says(s, &c, st(4, SFTP_FX_OK), "06%08x%s%016x%s", 4u, hh(0, 1), 0u, hs("hello"))
                   && says(s, &c, st(5, SFTP_FX_OK), "06%08x%s%016x%s", 5u, hh(0, 1), 5u,
                           hs(" world"))
                   && says(s, &c, st(6, SFTP_FX_OK), "06%08x%s%016x%s", 6u, hh(0, 1), 11u,
                           hs("")),
               "open-write: WRITEs at 0 and 5, and one of 0 bytes, are OK");
    test_check(says(s, &c, st(7, SFTP_FX_NO_SUCH_FILE),
                    "05%08x%s" "0000000000000000" "00000064", 7u, hh(0, 1)),
               "open-write: READ on a write-only handle is NO_SUCH_FILE (EBADF)");
    test_check(says(s, &c, st(8, SFTP_FX_BAD_MESSAGE),
                    "06%08x%s" "8000000000000000" "%s", 8u, hh(0, 1), hs("x")),
               "open-write: WRITE at an offset over INT64_MAX is BAD_MESSAGE");
    test_check(says(s, &c, st(9, SFTP_FX_OK), "04%08x%s", 9u, hh(0, 1))
                   && file_is(W_FS "\\new.txt", "hello world", 11),
               "open-write: CLOSE is OK and the file holds exactly what was written");
    test_check(says(s, &c, st(10, SFTP_FX_FAILURE), "%s",
                    op_open_hex(10, W "/new.txt",
                                SFTP_OPEN_WRITE | SFTP_OPEN_CREAT | SFTP_OPEN_EXCL)),
               "open-write: CREAT|EXCL on an existing file is FAILURE");
    test_check(says(s, &c, hd(11, 0, 2), "%s",
                    op_open_hex(11, W "/new.txt", SFTP_OPEN_READ | SFTP_OPEN_WRITE))
                   && says(s, &c, st(12, SFTP_FX_OK), "06%08x%s%016x%s", 12u, hh(0, 2), 6u,
                           hs("W"))
                   && says(s, &c, dt(13, "hello World"),
                           "05%08x%s" "0000000000000000" "00000064", 13u, hh(0, 2))
                   && says(s, &c, st(14, SFTP_FX_OK), "04%08x%s", 14u, hh(0, 2))
                   && file_is(W_FS "\\new.txt", "hello World", 11),
               "open-write: READ|WRITE without TRUNC overwrites in place and reads back");
    test_check(axl_file_set_contents(W_FS "\\new.txt", "xy", 2) == AXL_OK
                   && says(s, &c, hd(15, 0, 3), "%s",
                           op_open_hex(15, W "/new.txt", SFTP_OPEN_WRITE | SFTP_OPEN_APPEND))
                   && says(s, &c, st(16, SFTP_FX_OK), "06%08x%s%016x%s", 16u, hh(0, 3), 0u,
                           hs("ab"))
                   && says(s, &c, st(17, SFTP_FX_OK), "06%08x%s%016x%s", 17u, hh(0, 3), 0u,
                           hs("cd"))
                   && says(s, &c, st(18, SFTP_FX_OK), "04%08x%s", 18u, hh(0, 3))
                   && file_is(W_FS "\\new.txt", "xyabcd", 6),
               "open-write: APPEND writes at the end whatever the offset says");
    test_check(says(s, &c, hd(19, 0, 4), "%s",
                    op_open_hex(19, W "/new.txt", SFTP_OPEN_WRITE | SFTP_OPEN_TRUNC))
                   && says(s, &c, st(20, SFTP_FX_OK), "04%08x%s", 20u, hh(0, 4))
                   && file_is(W_FS "\\new.txt", "", 0),
               "open-write: TRUNC empties an existing file");
    test_check(axl_file_set_contents(W_FS "\\new.txt", "abc", 3) == AXL_OK
                   && says(s, &c, hd(21, 0, 5), "%s",
                           op_open_hex(21, W "/new.txt", SFTP_OPEN_READ))
                   && says(s, &c, st(22, SFTP_FX_NO_SUCH_FILE), "06%08x%s%016x%s", 22u,
                           hh(0, 5), 0u, hs("z"))
                   && says(s, &c, st(23, SFTP_FX_OK), "04%08x%s", 23u, hh(0, 5))
                   && file_is(W_FS "\\new.txt", "abc", 3),
               "open-write: WRITE on a read-only handle is NO_SUCH_FILE (EBADF), nothing written");
    test_check(says(s, &c, hd(24, 0, 6), "0b%08x%s", 24u, hs(W))
                   && says(s, &c, st(25, SFTP_FX_FAILURE), "06%08x%s%016x%s", 25u, hh(0, 6),
                           0u, hs("z"))
                   && says(s, &c, st(26, SFTP_FX_OK), "04%08x%s", 26u, hh(0, 6)),
               "open-write: WRITE on a directory handle is FAILURE");
    test_check(says(s, &c, st(27, SFTP_FX_FAILURE), "%s", op_open_hex(27, W, SFTP_OPEN_WRITE))
                   && says(s, &c, st(28, SFTP_FX_FAILURE), "%s",
                           op_open_hex(28, "/", SFTP_OPEN_WRITE))
                   && says(s, &c, st(29, SFTP_FX_FAILURE), "%s",
                           op_open_hex(29, "/fs0", SFTP_OPEN_WRITE)),
               "open-write: a directory, the root and a volume root are FAILURE (EISDIR)");
    test_check(says(s, &c, st(30, SFTP_FX_PERMISSION_DENIED), "%s",
                    op_open_hex(30, "/newtop", SFTP_OPEN_WRITE | SFTP_OPEN_CREAT))
                   && says(s, &c, st(31, SFTP_FX_NO_SUCH_FILE), "%s",
                           op_open_hex(31, "/newtop", SFTP_OPEN_WRITE)),
               "open-write: creating in the virtual root is PERMISSION_DENIED; without "
               "CREAT it is NO_SUCH_FILE");
    axl_file_delete(W_FS "\\conly.txt");
    test_check(says(s, &c, hd(32, 0, 7), "%s",
                    op_open_hex(32, W "/conly.txt", SFTP_OPEN_CREAT))
                   && says(s, &c, st(33, SFTP_FX_EOF),
                           "05%08x%s" "0000000000000000" "00000064", 33u, hh(0, 7))
                   && says(s, &c, st(34, SFTP_FX_OK), "04%08x%s", 34u, hh(0, 7))
                   && file_is(W_FS "\\conly.txt", "", 0),
               "open-write: CREAT alone creates an empty file opened for reading");
    test_check(!c.ended, "open-write: none of that ended the session");
    sftp_session_close(s);
    test_check(axl_file_delete(W_FS "\\conly.txt") == AXL_OK,
               "open-write: the created file is removed");
}

/* SETSTAT and FSETSTAT: a size is applied; permissions, times and owner are
   answered OK with no change, as sftp-server answers them on a volume that
   keeps none; the order of the failures is sftp-server's (each attribute in
   turn, the last failure's status sent). */
static void
test_session_setstat(void)
{
    static Cap c;
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(12, "no session");
        return;
    }
    test_check(axl_file_set_contents(W_FS "\\s.txt", "hello world", 11) == AXL_OK
                   && says(s, &c, st(1, SFTP_FX_OK), "09%08x%s" "00000001" "%016x", 1u,
                           hs(W "/s.txt"), 3u)
                   && file_is(W_FS "\\s.txt", "hel", 3),
               "setstat: a size shrinks the file");
    test_check(says(s, &c, st(2, SFTP_FX_OK), "09%08x%s" "00000001" "%016x", 2u,
                    hs(W "/s.txt"), 5u)
                   && file_is(W_FS "\\s.txt", "hel\0\0", 5),
               "setstat: a size grows it, with zeros");
    test_check(says(s, &c, st(3, SFTP_FX_OK), "09%08x%s" "00000006" "00000000" "00000000"
                    "000001a4", 3u, hs(W "/s.txt"))
                   && file_is(W_FS "\\s.txt", "hel\0\0", 5),
               "setstat: owner and permissions are OK, the file unchanged");
    test_check(says(s, &c, st(4, SFTP_FX_OK), "09%08x%s" "00000000", 4u, hs(W "/nope"))
                   && says(s, &c, st(5, SFTP_FX_NO_SUCH_FILE), "09%08x%s" "00000004"
                           "000001a4", 5u, hs(W "/nope"))
                   && says(s, &c, st(6, SFTP_FX_NO_SUCH_FILE), "09%08x%s" "00000001" "%016x",
                           6u, hs(W "/nope"), 1u)
                   && says(s, &c, st(7, SFTP_FX_NO_SUCH_FILE), "09%08x%s" "00000008"
                           "0000000100000001", 7u, hs(W "/nope"))
                   && !exists(W_FS "\\nope"),
               "setstat: on a missing path, no attributes is OK (nothing to do), any "
               "attribute NO_SUCH_FILE");
    test_check(says(s, &c, st(8, SFTP_FX_FAILURE), "09%08x%s" "00000001" "%016x", 8u, hs(W),
                    1u),
               "setstat: a size on a directory is FAILURE (EISDIR)");
    test_check(says(s, &c, st(9, SFTP_FX_PERMISSION_DENIED), "09%08x%s" "00000004" "000001ed",
                    9u, hs("/"))
                   && says(s, &c, st(10, SFTP_FX_PERMISSION_DENIED), "09%08x%s" "00000008"
                           "0000000100000001", 10u, hs("/"))
                   && says(s, &c, st(11, SFTP_FX_FAILURE), "09%08x%s" "00000001" "%016x",
                           11u, hs("/"), 1u)
                   && says(s, &c, st(12, SFTP_FX_PERMISSION_DENIED), "09%08x%s" "00000005"
                           "%016x" "000001ed", 12u, hs("/"), 1u)
                   && says(s, &c, st(13, SFTP_FX_OK), "09%08x%s" "00000000", 13u, hs("/")),
               "setstat: the virtual root's permissions and times are PERMISSION_DENIED "
               "(not the owner's), its size FAILURE, the last failure wins");
    test_check(says(s, &c, st(14, SFTP_FX_OK), "09%08x%s" "00000004" "000001ed", 14u,
                    hs("/fs0"))
                   && says(s, &c, st(15, SFTP_FX_FAILURE), "09%08x%s" "00000001" "%016x",
                           15u, hs("/fs0"), 1u),
               "setstat: a volume root's permissions are OK, its size FAILURE");
    /* FSETSTAT: on a write handle a size applies; on a read handle it is
       BAD_MESSAGE (ftruncate's EINVAL) and permissions are still OK; a
       directory handle and a stale one are FAILURE. */
    test_check(says(s, &c, hd(16, 0, 1), "%s", op_open_hex(16, W "/s.txt", SFTP_OPEN_WRITE))
                   && says(s, &c, st(17, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 17u,
                           hh(0, 1), 2u)
                   && file_is(W_FS "\\s.txt", "he", 2),
               "fsetstat: a size on a write handle truncates");
    test_check(says(s, &c, hd(18, 1, 1), "%s", op_open_hex(18, W "/s.txt", SFTP_OPEN_READ))
                   && says(s, &c, st(19, SFTP_FX_BAD_MESSAGE), "0a%08x%s" "00000001" "%016x",
                           19u, hh(1, 1), 1u)
                   && says(s, &c, st(20, SFTP_FX_OK), "0a%08x%s" "00000004" "000001a4", 20u,
                           hh(1, 1))
                   && file_is(W_FS "\\s.txt", "he", 2),
               "fsetstat: a size on a read handle is BAD_MESSAGE, permissions OK");
    test_check(says(s, &c, hd(21, 2, 1), "0b%08x%s", 21u, hs(W))
                   && says(s, &c, st(22, SFTP_FX_FAILURE), "0a%08x%s" "00000000", 22u,
                           hh(2, 1))
                   && says(s, &c, st(23, SFTP_FX_FAILURE), "0a%08x%s" "00000000", 23u,
                           hh(5, 1)),
               "fsetstat: a directory handle and an unknown handle are FAILURE");
    test_check(!c.ended, "setstat: none of that ended the session");
    sftp_session_close(s);
    test_check(axl_file_delete(W_FS "\\s.txt") == AXL_OK, "setstat: the file is removed");
}

/* MKDIR, REMOVE, RMDIR, RENAME: statuses as sftp-server's for the same
   condition. The virtual root is a directory the user does not own: making
   a name in it is PERMISSION_DENIED, as is removing or renaming a volume
   (a mount point in it); `/` itself is FAILURE (EBUSY, EEXIST, EISDIR). */
static void
test_session_namespace(void)
{
    static Cap c;
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(19, "no session");
        return;
    }
    test_check(axl_file_set_contents(W_FS "\\f.txt", "f", 1) == AXL_OK,
               "namespace: the fixture is written");
    test_check(says(s, &c, st(1, SFTP_FX_OK), "0e%08x%s00000000", 1u, hs(W "/d"))
                   && is_dir(W_FS "\\d"),
               "mkdir: makes a directory");
    test_check(says(s, &c, st(2, SFTP_FX_FAILURE), "0e%08x%s00000000", 2u, hs(W "/d"))
                   && says(s, &c, st(3, SFTP_FX_FAILURE), "0e%08x%s00000000", 3u,
                           hs(W "/f.txt"))
                   && says(s, &c, st(4, SFTP_FX_NO_SUCH_FILE), "0e%08x%s00000000", 4u,
                           hs(W "/nodir/x"))
                   && says(s, &c, st(5, SFTP_FX_NO_SUCH_FILE), "0e%08x%s00000000", 5u,
                           hs(W "/f.txt/x"))
                   && is_dir(W_FS "\\d") && file_is(W_FS "\\f.txt", "f", 1),
               "mkdir: an existing directory or file is FAILURE (EEXIST); a missing parent "
               "or a file as parent NO_SUCH_FILE");
    test_check(says(s, &c, st(6, SFTP_FX_FAILURE), "0e%08x%s00000000", 6u, hs("/"))
                   && says(s, &c, st(7, SFTP_FX_FAILURE), "0e%08x%s00000000", 7u, hs("/fs0"))
                   && says(s, &c, st(8, SFTP_FX_PERMISSION_DENIED), "0e%08x%s00000000", 8u,
                           hs("/newtop")),
               "mkdir: / and a volume are FAILURE (EEXIST); a new name in / is "
               "PERMISSION_DENIED");
    test_check(axl_file_set_contents(W_FS "\\d\\x.txt", "x", 1) == AXL_OK
                   && says(s, &c, st(9, SFTP_FX_FAILURE), "0d%08x%s", 9u, hs(W "/d"))
                   && says(s, &c, st(10, SFTP_FX_NO_SUCH_FILE), "0d%08x%s", 10u, hs(W "/nope"))
                   && says(s, &c, st(11, SFTP_FX_NO_SUCH_FILE), "0d%08x%s", 11u,
                           hs(W "/nodir/x"))
                   && says(s, &c, st(12, SFTP_FX_FAILURE), "0d%08x%s", 12u, hs("/"))
                   && says(s, &c, st(13, SFTP_FX_PERMISSION_DENIED), "0d%08x%s", 13u,
                           hs("/fs0"))
                   && says(s, &c, st(14, SFTP_FX_NO_SUCH_FILE), "0d%08x%s", 14u, hs("/newtop"))
                   && is_dir(W_FS "\\d"),
               "remove: a directory is FAILURE (EISDIR), a missing name NO_SUCH_FILE, / "
               "FAILURE, a volume PERMISSION_DENIED");
    test_check(says(s, &c, st(15, SFTP_FX_FAILURE), "0f%08x%s", 15u, hs(W "/d"))
                   && says(s, &c, st(16, SFTP_FX_NO_SUCH_FILE), "0f%08x%s", 16u,
                           hs(W "/f.txt"))
                   && says(s, &c, st(17, SFTP_FX_NO_SUCH_FILE), "0f%08x%s", 17u, hs(W "/nope"))
                   && says(s, &c, st(18, SFTP_FX_FAILURE), "0f%08x%s", 18u, hs("/"))
                   && says(s, &c, st(19, SFTP_FX_PERMISSION_DENIED), "0f%08x%s", 19u,
                           hs("/fs0"))
                   && says(s, &c, st(20, SFTP_FX_NO_SUCH_FILE), "0f%08x%s", 20u,
                           hs("/newtop"))
                   && is_dir(W_FS "\\d") && file_is(W_FS "\\f.txt", "f", 1),
               "rmdir: a non-empty directory is FAILURE, a file or a missing name "
               "NO_SUCH_FILE (ENOTDIR, ENOENT), / FAILURE, a volume PERMISSION_DENIED");
    test_check(says(s, &c, st(21, SFTP_FX_OK), "0d%08x%s", 21u, hs(W "/d/x.txt"))
                   && !exists(W_FS "\\d\\x.txt")
                   && says(s, &c, st(22, SFTP_FX_OK), "0f%08x%s", 22u, hs(W "/d"))
                   && !exists(W_FS "\\d"),
               "remove + rmdir: a file and then its emptied directory go");
    test_check(says(s, &c, st(23, SFTP_FX_OK), "12%08x%s%s", 23u, hs(W "/f.txt"),
                    hs(W "/g.txt"))
                   && !exists(W_FS "\\f.txt") && file_is(W_FS "\\g.txt", "f", 1),
               "rename: within a directory");
    test_check(axl_file_set_contents(W_FS "\\h.txt", "h", 1) == AXL_OK
                   && says(s, &c, st(24, SFTP_FX_FAILURE), "12%08x%s%s", 24u, hs(W "/g.txt"),
                           hs(W "/h.txt"))
                   && says(s, &c, st(25, SFTP_FX_FAILURE), "12%08x%s%s", 25u, hs(W "/g.txt"),
                           hs(W "/g.txt"))
                   && says(s, &c, st(26, SFTP_FX_FAILURE), "12%08x%s%s", 26u, hs(W "/g.txt"),
                           hs("/fs0"))
                   && file_is(W_FS "\\g.txt", "f", 1) && file_is(W_FS "\\h.txt", "h", 1),
               "rename: onto an existing name (itself, a volume) is FAILURE, as "
               "SSH_FXP_RENAME never replaces");
    test_check(says(s, &c, st(27, SFTP_FX_NO_SUCH_FILE), "12%08x%s%s", 27u, hs(W "/nope"),
                    hs(W "/x"))
                   && says(s, &c, st(28, SFTP_FX_NO_SUCH_FILE), "12%08x%s%s", 28u,
                           hs(W "/g.txt"), hs(W "/nodir/x"))
                   && says(s, &c, st(29, SFTP_FX_NO_SUCH_FILE), "12%08x%s%s", 29u,
                           hs(W "/g.txt"), hs(W "/h.txt/x"))
                   && says(s, &c, st(30, SFTP_FX_PERMISSION_DENIED), "12%08x%s%s", 30u,
                           hs(W "/g.txt"), hs("/newtop"))
                   && says(s, &c, st(31, SFTP_FX_FAILURE), "12%08x%s%s", 31u, hs("/"),
                           hs(W "/r"))
                   && says(s, &c, st(32, SFTP_FX_PERMISSION_DENIED), "12%08x%s%s", 32u,
                           hs("/fs0"), hs(W "/r"))
                   && file_is(W_FS "\\g.txt", "f", 1) && !exists(W_FS "\\r"),
               "rename: a missing source or target directory is NO_SUCH_FILE; into / or "
               "of a volume PERMISSION_DENIED; of / FAILURE");
    /* An EMPTY directory: the firmware's delete would take it, so only the
       server's own check keeps REMOVE from acting as RMDIR. */
    test_check(says(s, &c, st(33, SFTP_FX_OK), "0e%08x%s00000000", 33u, hs(W "/d2"))
                   && says(s, &c, st(37, SFTP_FX_FAILURE), "0d%08x%s", 37u, hs(W "/d2"))
                   && is_dir(W_FS "\\d2"),
               "remove: an empty directory is FAILURE (EISDIR) and stays");
    test_check(says(s, &c, st(34, SFTP_FX_OK), "12%08x%s%s", 34u, hs(W "/d2"),
                           hs(W "/d3"))
                   && is_dir(W_FS "\\d3") && !exists(W_FS "\\d2"),
               "rename: a directory within its directory");
    /* Between directories of one volume: moved, in one step. */
    test_check(says(s, &c, st(35, SFTP_FX_OK), "12%08x%s%s", 35u, hs(W "/g.txt"),
                    hs(W "/d3/g.txt"))
                   && !exists(W_FS "\\g.txt") && file_is(W_FS "\\d3\\g.txt", "f", 1),
               "rename: between directories moves the file");
    test_check(axl_dir_mkdir(W_FS "\\d4") == AXL_OK
                   && says(s, &c, st(38, SFTP_FX_OK), "12%08x%s%s", 38u, hs(W "/d3"),
                           hs(W "/d4/d3"))
                   && !exists(W_FS "\\d3") && file_is(W_FS "\\d4\\d3\\g.txt", "f", 1),
               "rename: a directory moves with what is in it");
    test_check(says(s, &c, st(39, SFTP_FX_BAD_MESSAGE), "12%08x%s%s", 39u, hs(W "/d4"),
                    hs(W "/d4/d3/d4"))
                   && says(s, &c, st(40, SFTP_FX_BAD_MESSAGE), "12%08x%s%s", 40u,
                           hs(W "/d4"), hs(W "/D4/x"))
                   && is_dir(W_FS "\\d4\\d3"),
               "rename: a directory into itself or below is BAD_MESSAGE (EINVAL), any case");
    /* FAT names are case-insensitive: "G.TXT" is the existing "g.txt". */
    test_check(says(s, &c, st(41, SFTP_FX_FAILURE), "12%08x%s%s", 41u, hs(W "/d4/d3/g.txt"),
                    hs(W "/d4/d3/G.TXT"))
                   && file_is(W_FS "\\d4\\d3\\g.txt", "f", 1),
               "rename: a case-only rename is FAILURE (the name exists), nothing changed");
    test_check(says(s, &c, st(36, SFTP_FX_OP_UNSUPPORTED), "14%08x%s%s", 36u, hs(W "/ln"),
                    hs(W "/g.txt")),
               "symlink: OP_UNSUPPORTED (FAT has no links)");
    test_check(!c.ended, "namespace: none of that ended the session");
    sftp_session_close(s);
    test_check(axl_file_delete(W_FS "\\d4\\d3\\g.txt") == AXL_OK
                   && axl_file_delete(W_FS "\\h.txt") == AXL_OK
                   && axl_dir_rmdir(W_FS "\\d4\\d3") == AXL_OK
                   && axl_dir_rmdir(W_FS "\\d4") == AXL_OK,
               "namespace: the fixtures are removed");
}

/* With every handle taken, OPEN for writing is FAILURE and creates nothing:
   the slot is taken before anything changes on the volume. */
static void
test_session_cap_creates_nothing(void)
{
    static Cap c;
    bool       all = true;
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(1, "no session");
        return;
    }
    axl_file_delete(W_FS "\\cap.txt");
    for (uint32_t i = 0; i < 16; i++) {
        all = says(s, &c, hd(400 + i, i, 1), "0b%08x%s", 400u + i, hs(W)) && all;
    }
    test_check(all
                   && says(s, &c, st(416, SFTP_FX_FAILURE), "%s",
                           op_open_hex(416, W "/cap.txt",
                                       SFTP_OPEN_WRITE | SFTP_OPEN_CREAT | SFTP_OPEN_TRUNC))
                   && !exists(W_FS "\\cap.txt"),
               "open-write: with 16 handles open, CREAT is FAILURE and creates nothing");
    sftp_session_close(s);
}

/* SETSTAT/FSETSTAT ACMODTIME: the mtime is applied (FAT keeps even seconds,
   so an odd one is stored one lower) and the atime ignored (FAT keeps no
   time of day for it). A time FAT cannot hold is BAD_MESSAGE, as
   sftp-server answers utimes' EINVAL; a read-only file or handle is
   PERMISSION_DENIED. `put -p`'s order -- WRITE, FSETSTAT, CLOSE -- keeps
   the time. */
static void
test_session_times(void)
{
    static Cap c;
    /* ACMODTIME with atime 5 and mtime @a m, as hex. */
#define T_ATTRS(m)  "00000008" "00000005" m

    axl_file_delete(W_FS "\\t.txt");
    test_check(axl_file_set_contents(W_FS "\\t.txt", "x", 1) == AXL_OK,
               "times: the fixture is written");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(11, "no session");
        return;
    }
    test_check(says(s, &c, st(1, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca01"), 1u,
                    hs(W "/t.txt"))
                   && mtime_of(W_FS "\\t.txt") == 1000000000u,
               "times: mtime 1000000001 is stored as 1000000000 (FAT's two seconds)");
    test_check(says(s, &c, st(2, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca02"), 2u,
                    hs(W "/t.txt"))
                   && mtime_of(W_FS "\\t.txt") == 1000000002u,
               "times: an even mtime is stored exactly");
    /* Before the first time the FAT driver can keep, the time is clamped to
       it, 1981-01-01 (EDK2 cannot store a 1980 year): `put -p` of an
       epoch-stamped file must not fail. 315532800 is 1980-01-01. */
    test_check(says(s, &c, st(3, SFTP_FX_OK), "09%08x%s" T_ATTRS("00000000"), 3u,
                    hs(W "/t.txt"))
                   && mtime_of(W_FS "\\t.txt") == 347155200u,
               "times: 0 is stored as 347155200 (1981-01-01), OK");
    test_check(says(s, &c, st(2, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca02"), 2u,
                    hs(W "/t.txt"))
                   && says(s, &c, st(4, SFTP_FX_OK), "09%08x%s" T_ATTRS("12cea5ff"),
                           4u, hs(W "/t.txt"))
                   && says(s, &c, st(5, SFTP_FX_OK), "09%08x%s" T_ATTRS("12cea600"),
                           5u, hs(W "/t.txt"))
                   && mtime_of(W_FS "\\t.txt") == 347155200u,
               "times: 315532799 and 315532800 (1980) are stored as 347155200, OK");
    test_check(says(s, &c, st(2, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca02"), 2u,
                    hs(W "/t.txt"))
                   && mtime_of(W_FS "\\t.txt") == 1000000002u,
               "times: and back to 1000000002");
    uint64_t ro_before = mtime_of(RO_FS);
    test_check(ro_before != 0
                   && says(s, &c, st(5, SFTP_FX_PERMISSION_DENIED), "09%08x%s" T_ATTRS("3b9aca02"),
                           5u, hs("/fs0/sftp-ro.txt"))
                   && mtime_of(RO_FS) == ro_before,
               "times: a read-only file is PERMISSION_DENIED, its time unchanged");
    test_check(says(s, &c, st(6, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca04"), 6u, hs(W))
                   && mtime_of(W_FS) == 1000000004u
                   && says(s, &c, st(7, SFTP_FX_OK), "09%08x%s" T_ATTRS("3b9aca04"), 7u,
                           hs("/fs0")),
               "times: a directory takes a time; a volume root, which has no entry, is OK");
    /* put -p: OPEN, WRITE, FSETSTAT, CLOSE. */
    test_check(says(s, &c, hd(8, 0, 1), "%s",
                    op_open_hex(8, W "/t.txt",
                                SFTP_OPEN_WRITE | SFTP_OPEN_CREAT | SFTP_OPEN_TRUNC))
                   && says(s, &c, st(9, SFTP_FX_OK), "06%08x%s%016x%s", 9u, hh(0, 1), 0u,
                           hs("data"))
                   && says(s, &c, st(10, SFTP_FX_OK), "0a%08x%s" T_ATTRS("3b9aca07"), 10u,
                           hh(0, 1))
                   && says(s, &c, st(11, SFTP_FX_OK), "04%08x%s", 11u, hh(0, 1))
                   && file_is(W_FS "\\t.txt", "data", 4)
                   && mtime_of(W_FS "\\t.txt") == 1000000006u,
               "times: WRITE, FSETSTAT, CLOSE keeps the time set (put -p's order)");
    test_check(says(s, &c, hd(12, 0, 2), "%s", op_open_hex(12, W "/t.txt", SFTP_OPEN_READ))
                   && says(s, &c, st(13, SFTP_FX_PERMISSION_DENIED),
                           "0a%08x%s" T_ATTRS("3b9aca02"), 13u, hh(0, 2))
                   && says(s, &c, st(14, SFTP_FX_OK), "04%08x%s", 14u, hh(0, 2))
                   && mtime_of(W_FS "\\t.txt") == 1000000006u,
               "times: FSETSTAT on a handle opened for reading is PERMISSION_DENIED");
    test_check(!c.ended, "times: none of that ended the session");
    sftp_session_close(s);
    test_check(axl_file_delete(W_FS "\\t.txt") == AXL_OK, "times: the file is removed");
#undef T_ATTRS
}

/* FSTAT and FSETSTAT act on the file the handle was opened on, wherever it
   now is -- not on a path saved at OPEN, which after a rename and a new
   file at the old name would truncate the wrong file. */
static void
test_session_handle_follows_file(void)
{
    static Cap c;
    /* ATTRS for id @a id: flags SIZE|PERMISSIONS|ACMODTIME, the size,
       0100644, and atime = mtime = 1000000000, the time stamped below,
       which a rename and a truncate both preserve. */
    static const char ATTRS_FMT[] = "0000001d69%08x0000000d%016x000081a43b9aca003b9aca00";
    char want[96];

    axl_file_delete(W_FS "\\a.txt");
    axl_file_delete(W_FS "\\b.txt");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(5, "no session");
        return;
    }
    test_check(axl_file_set_contents(W_FS "\\a.txt", "hello", 5) == AXL_OK
                   && axl_file_set_mtime(W_FS "\\a.txt", 1000000000u) == AXL_OK
                   && says(s, &c, hd(1, 0, 1), "%s",
                           op_open_hex(1, W "/a.txt", SFTP_OPEN_WRITE))
                   && says(s, &c, st(2, SFTP_FX_OK), "12%08x%s%s", 2u, hs(W "/a.txt"),
                           hs(W "/b.txt"))
                   && axl_file_set_contents(W_FS "\\a.txt", "fresh!", 6) == AXL_OK,
               "follows: a.txt stamped and opened for writing, renamed to b.txt, a new a.txt made");
    axl_snprintf(want, sizeof(want), ATTRS_FMT, 3u, 5u);
    test_check(says(s, &c, want, "08%08x%s", 3u, hh(0, 1)),
               "follows: FSTAT reports the renamed file (5 bytes), not the new one");
    test_check(says(s, &c, st(4, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 4u, hh(0, 1),
                    0u)
                   && file_is(W_FS "\\b.txt", "", 0) && file_is(W_FS "\\a.txt", "fresh!", 6),
               "follows: FSETSTAT size 0 truncates the renamed file; the new one is untouched");
    axl_snprintf(want, sizeof(want), ATTRS_FMT, 5u, 0u);
    test_check(says(s, &c, want, "08%08x%s", 5u, hh(0, 1))
                   && says(s, &c, st(6, SFTP_FX_OK), "04%08x%s", 6u, hh(0, 1)),
               "follows: FSTAT then sees it empty; CLOSE is OK");
    test_check(!c.ended, "follows: none of that ended the session");
    sftp_session_close(s);
    test_check(axl_file_delete(W_FS "\\a.txt") == AXL_OK
                   && axl_file_delete(W_FS "\\b.txt") == AXL_OK,
               "follows: the files are removed");
}

/* A file with the FAT read-only attribute (test-axl.sh stages it with
   mattrib +r): every write to it is PERMISSION_DENIED -- the attribute is
   its mode 0444, and FAT under UEFI also refuses to delete or rename it --
   and it is left exactly as it was. */

static void
test_session_ro_attr(void)
{
    static Cap c;
    AxlFsEntry e;

    test_check(axl_file_info(RO_FS, &e) == AXL_OK && axl_fs_entry_is_read_only(&e),
               "ro-attr: the fixture carries the read-only attribute");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(5, "no session");
        return;
    }
    test_check(says(s, &c, st(1, SFTP_FX_PERMISSION_DENIED), "0d%08x%s", 1u,
                    hs("/fs0/sftp-ro.txt")),
               "ro-attr: REMOVE is PERMISSION_DENIED");
    test_check(says(s, &c, st(2, SFTP_FX_PERMISSION_DENIED), "12%08x%s%s", 2u,
                    hs("/fs0/sftp-ro.txt"), hs("/fs0/sftp-ro2.txt"))
                   && !exists("fs0:\\sftp-ro2.txt"),
               "ro-attr: RENAME is PERMISSION_DENIED, nothing renamed");
    test_check(says(s, &c, st(3, SFTP_FX_PERMISSION_DENIED), "%s",
                    op_open_hex(3, "/fs0/sftp-ro.txt", SFTP_OPEN_WRITE | SFTP_OPEN_TRUNC))
                   && says(s, &c, st(4, SFTP_FX_PERMISSION_DENIED), "09%08x%s" "00000001"
                           "%016x", 4u, hs("/fs0/sftp-ro.txt"), 0u),
               "ro-attr: OPEN for writing and a size are PERMISSION_DENIED");
    test_check(file_is(RO_FS, "locked\n", 7), "ro-attr: the file is untouched");
    test_check(!c.ended, "ro-attr: none of that ended the session");
    sftp_session_close(s);
}

/* posix-rename@openssh.com's name, as an SFTP string in hex. */
/* The posix-rename tests' directory, both forms. */
#define PRD_FS  W_FS "\\pr"
#define PRD     W "/pr"
#define S_PR "00000018706f7369782d72656e616d65406f70656e7373682e636f6d"

/* posix-rename of @a from to @a to, id @a id, answered @a code. */
static bool
pr_says(SftpSession *s, Cap *c, uint32_t id, uint32_t code, const char *from, const char *to)
{
    return says(s, c, st(id, code), "c8%08x" S_PR "%s%s", (unsigned)id, hs(from), hs(to));
}

/* Whether directory @a fs holds no entry whose name starts with '~': no
   set-aside copy of a replaced target was left behind. */
static bool
no_aside(const char *fs)
{
    AxlDir    *d  = axl_dir_open(fs);
    bool       ok = d != NULL;
    AxlFsEntry e;

    while (ok) {
        e.struct_size = sizeof(e);
        e.version     = AXL_FS_ENTRY_VERSION;
        if (!axl_dir_read(d, &e)) {
            break;
        }
        if (e.name[0] == '~') {
            axl_printf("    left behind: %s\\%s\n", fs, e.name);
            ok = false;
        }
    }
    axl_dir_close(d);
    return ok;
}

static int
cmp_name(const void *a, const void *b)
{
    return axl_strcmp((const char *)a, (const char *)b);
}

/* Whether directory @a fs holds exactly the names @a want ("a|b|c", sorted
   bytewise), spelled as the filesystem stores them -- letter case
   included. `.` and `..` are not counted. */
static bool
names_are(const char *fs, const char *want)
{
    static char names[16][256];
    char        got[1024];
    size_t      n  = 0;
    size_t      k  = 0;
    AxlDir     *d  = axl_dir_open(fs);
    AxlFsEntry  e;

    if (d == NULL) {
        return false;
    }
    for (;;) {
        e.struct_size = sizeof(e);
        e.version     = AXL_FS_ENTRY_VERSION;
        if (!axl_dir_read(d, &e)) {
            break;
        }
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0 || n == 16) {
            continue;
        }
        axl_strlcpy(names[n++], e.name, sizeof(names[0]));
    }
    axl_dir_close(d);
    axl_qsort(names, n, sizeof(names[0]), cmp_name);
    got[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        k += (size_t)axl_snprintf(got + k, sizeof(got) - k, "%s%s", i > 0 ? "|" : "", names[i]);
    }
    bool ok = axl_strcmp(got, want) == 0;
    if (!ok) {
        axl_printf("    %s holds [%s], want [%s]\n", fs, got, want);
    }
    return ok;
}

/* posix-rename@openssh.com: rename(2) -- it REPLACES an existing target,
   where SSH_FXP_RENAME never does. Statuses are host sftp-server's for the
   same condition (OpenSSH 9.9p1, driven on stdin): a file onto a file, onto
   itself, and a directory onto an EMPTY directory are OK; a file onto a
   directory is FAILURE (EISDIR), a directory onto a file NO_SUCH_FILE
   (ENOTDIR), onto a non-empty directory FAILURE (ENOTEMPTY), into itself
   BAD_MESSAGE (EINVAL); a missing source or target directory NO_SUCH_FILE.
   A replaced target is set aside, the source moved, and only then the
   aside removed, so no failure loses both. */
static void
test_session_posix_rename(void)
{
    static Cap c;

    test_check(rm_tree(PRD_FS) && axl_dir_mkdir(PRD_FS) == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\a.txt", "A", 1) == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\b.txt", "B", 1) == AXL_OK
                   && axl_dir_mkdir(PRD_FS "\\d1") == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\d1\\m.txt", "m", 1) == AXL_OK
                   && axl_dir_mkdir(PRD_FS "\\d2") == AXL_OK
                   && axl_dir_mkdir(PRD_FS "\\full") == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\full\\x.txt", "x", 1) == AXL_OK,
               "posix-rename: the fixture is written");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(20, "no session");
        return;
    }
    test_check(pr_says(s, &c, 1, SFTP_FX_OK, PRD "/a.txt", PRD "/b.txt")
                   && !exists(PRD_FS "\\a.txt") && file_is(PRD_FS "\\b.txt", "A", 1)
                   && no_aside(PRD_FS),
               "posix-rename: a file onto a file replaces it, nothing set aside is left");
    test_check(pr_says(s, &c, 2, SFTP_FX_OK, PRD "/b.txt", PRD "/b.txt")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && no_aside(PRD_FS),
               "posix-rename: onto itself is OK and changes nothing, as rename(2) does");
    test_check(pr_says(s, &c, 3, SFTP_FX_OK, PRD "/b.txt", PRD "/B.TXT")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && no_aside(PRD_FS)
                   && names_are(PRD_FS, "B.TXT|d1|d2|full"),
               "posix-rename: a case-only change is OK and the entry is now spelled B.TXT, "
               "as rename(2) on a case-preserving filesystem does");
    /* B1: an 8.3 alias of the source -- or of a directory on the way -- is
       the same entry: rename(2) of a file onto itself changes nothing. */
    test_check(axl_dir_mkdir(PRD_FS "\\al") == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\al\\longfilename.txt", "L", 1) == AXL_OK
                   && axl_dir_mkdir(PRD_FS "\\al\\longdirname") == AXL_OK
                   && axl_file_set_contents(PRD_FS "\\al\\longdirname\\x.txt", "X", 1)
                          == AXL_OK,
               "posix-rename: a long-named file and directory are written");
    test_check(pr_says(s, &c, 24, SFTP_FX_OK, PRD "/al/longfilename.txt", PRD "/al/LONGFI~1.TXT")
                   && names_are(PRD_FS "\\al", "longdirname|longfilename.txt")
                   && file_is(PRD_FS "\\al\\longfilename.txt", "L", 1),
               "posix-rename: onto its own 8.3 alias is OK and changes nothing -- the long "
               "name stays, exactly");
    test_check(pr_says(s, &c, 25, SFTP_FX_OK, PRD "/al/longdirname/x.txt",
                       PRD "/al/LONGDI~1/x.txt")
                   && names_are(PRD_FS "\\al\\longdirname", "x.txt")
                   && names_are(PRD_FS "\\al", "longdirname|longfilename.txt")
                   && file_is(PRD_FS "\\al\\longdirname\\x.txt", "X", 1),
               "posix-rename: through its directory's 8.3 alias is the same entry: OK, "
               "nothing changed");
    test_check(rm_tree(PRD_FS "\\al"), "posix-rename: the long-named fixture is removed");
    test_check(pr_says(s, &c, 4, SFTP_FX_NO_SUCH_FILE, PRD "/nope", PRD "/z")
                   && pr_says(s, &c, 5, SFTP_FX_NO_SUCH_FILE, PRD "/b.txt", PRD "/nodir/x")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && !exists(PRD_FS "\\z"),
               "posix-rename: a missing source or target directory is NO_SUCH_FILE");
    /* rename(2) looks the old entry up first: a missing source is ENOENT
       even onto a new name in `/` or onto a volume, as RENAME answers. */
    test_check(pr_says(s, &c, 26, SFTP_FX_NO_SUCH_FILE, PRD "/nope", "/newtop")
                   && pr_says(s, &c, 27, SFTP_FX_NO_SUCH_FILE, PRD "/nope", "/fs0"),
               "posix-rename: a missing source onto a new name in / or onto a volume is "
               "NO_SUCH_FILE (the source is looked up first)");
    test_check(pr_says(s, &c, 6, SFTP_FX_FAILURE, PRD "/b.txt", PRD "/d1")
                   && pr_says(s, &c, 7, SFTP_FX_NO_SUCH_FILE, PRD "/d1", PRD "/b.txt")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && file_is(PRD_FS "\\d1\\m.txt", "m", 1),
               "posix-rename: a file onto a directory is FAILURE (EISDIR), a directory onto "
               "a file NO_SUCH_FILE (ENOTDIR), both unchanged");
    test_check(pr_says(s, &c, 8, SFTP_FX_FAILURE, PRD "/d1", PRD "/full")
                   && file_is(PRD_FS "\\d1\\m.txt", "m", 1)
                   && file_is(PRD_FS "\\full\\x.txt", "x", 1),
               "posix-rename: a directory onto a non-empty one is FAILURE (ENOTEMPTY)");
    test_check(pr_says(s, &c, 9, SFTP_FX_BAD_MESSAGE, PRD "/d1", PRD "/d1/sub")
                   && is_dir(PRD_FS "\\d1"),
               "posix-rename: a directory into itself is BAD_MESSAGE (EINVAL)");
    /* The move itself refused AFTER the target was set aside: the FAT driver
       will not move a directory with a file below it open. The target must
       come back, and nothing be left aside. */
    test_check(says(s, &c, hd(20, 0, 1), "%s", op_open_hex(20, PRD "/d1/m.txt", SFTP_OPEN_READ))
                   && pr_says(s, &c, 21, SFTP_FX_FAILURE, PRD "/d1", PRD "/d2")
                   && is_dir(PRD_FS "\\d2") && !exists(PRD_FS "\\d2\\m.txt")
                   && file_is(PRD_FS "\\d1\\m.txt", "m", 1) && no_aside(PRD_FS)
                   && says(s, &c, st(22, SFTP_FX_OK), "04%08x%s", 22u, hh(0, 1)),
               "posix-rename: a move the driver refuses after the target was set aside "
               "puts the target back: FAILURE, both as they were");
    test_check(pr_says(s, &c, 10, SFTP_FX_OK, PRD "/d1", PRD "/d2")
                   && !exists(PRD_FS "\\d1") && file_is(PRD_FS "\\d2\\m.txt", "m", 1)
                   && no_aside(PRD_FS),
               "posix-rename: a directory onto an empty directory replaces it");
    test_check(pr_says(s, &c, 11, SFTP_FX_PERMISSION_DENIED, PRD "/b.txt", "/fs0/sftp-ro.txt")
                   && pr_says(s, &c, 12, SFTP_FX_PERMISSION_DENIED, "/fs0/sftp-ro.txt",
                              PRD "/r.txt")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && file_is(RO_FS, "locked\n", 7)
                   && !exists(PRD_FS "\\r.txt") && no_aside("fs0:\\"),
               "posix-rename: onto or of a file with the read-only attribute is "
               "PERMISSION_DENIED, both unchanged (it could not be removed or moved)");
    test_check(pr_says(s, &c, 13, SFTP_FX_FAILURE, "/", PRD "/r")
                   && pr_says(s, &c, 14, SFTP_FX_PERMISSION_DENIED, "/fs0", PRD "/r")
                   && pr_says(s, &c, 15, SFTP_FX_PERMISSION_DENIED, PRD "/b.txt", "/newtop")
                   && pr_says(s, &c, 16, SFTP_FX_FAILURE, PRD "/b.txt", "/fs0")
                   && pr_says(s, &c, 17, SFTP_FX_FAILURE, PRD "/d2", "/")
                   && file_is(PRD_FS "\\b.txt", "A", 1) && !exists(PRD_FS "\\r"),
               "posix-rename: of / FAILURE, of a volume PERMISSION_DENIED, into / "
               "PERMISSION_DENIED, onto a volume or / FAILURE");
    test_check(pr_says(s, &c, 18, SFTP_FX_OK, PRD "/b.txt", PRD "/c.txt")
                   && !exists(PRD_FS "\\b.txt") && file_is(PRD_FS "\\c.txt", "A", 1),
               "posix-rename: onto a new name is a plain rename");
    test_check(pr_says(s, &c, 19, SFTP_FX_OK, PRD "/c.txt", PRD "/full/x.txt")
                   && !exists(PRD_FS "\\c.txt") && file_is(PRD_FS "\\full\\x.txt", "A", 1)
                   && no_aside(PRD_FS "\\full"),
               "posix-rename: onto a file in another directory replaces it there");
    test_check(!c.ended, "posix-rename: none of that ended the session");
    sftp_session_close(s);
    test_check(rm_tree(PRD_FS), "posix-rename: the fixture is removed");
}

/* The size a path holds, or UINT64_MAX when it cannot be read. */
static uint64_t
size_of(const char *fs)
{
    AxlFsEntry e;

    return axl_file_info(fs, &e) == AXL_OK ? e.size : UINT64_MAX;
}

/* SFTP_GROW_MAX: a size, or a WRITE's gap past the end, that grows a file
   by more than it is FAILURE with the file unchanged; exactly it is done.
   Measured from the file's size at the time, so a 5-byte file may reach
   5 + SFTP_GROW_MAX. A WRITE whose offset a READ past the end already
   seeked to is judged the same way, not waved through as "in position". */
static void
test_session_grow(void)
{
    static Cap c;
    const uint32_t MAX = SFTP_GROW_MAX;
    const char    *G   = W "/gr/g.bin";
    const char    *GF  = W_FS "\\gr\\g.bin";

    test_check(rm_tree(W_FS "\\gr") && axl_dir_mkdir(W_FS "\\gr") == AXL_OK
                   && axl_file_set_contents(GF, "abcde", 5) == AXL_OK,
               "grow: a 5-byte file is written");
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(11, "no session");
        return;
    }
    test_check(says(s, &c, st(1, SFTP_FX_FAILURE), "09%08x%s" "00000001" "%016x", 1u, hs(G),
                    5u + MAX + 1u)
                   && size_of(GF) == 5u,
               "grow: SETSTAT growing by SFTP_GROW_MAX + 1 is FAILURE, the size unchanged");
    /* sftp-server applies each attribute in turn whatever the one before
       it did: the refused size changes nothing, the time is still set. */
    test_check(says(s, &c, st(30, SFTP_FX_FAILURE), "09%08x%s" "00000009" "%016x"
                    "3b9aca00" "3b9aca00", 30u, hs(G), 5u + MAX + 1u)
                   && size_of(GF) == 5u && mtime_of(GF) == 1000000000u,
               "grow: a refused size leaves the size unchanged; a time in the same "
               "SETSTAT is still set");
    test_check(says(s, &c, st(2, SFTP_FX_OK), "09%08x%s" "00000001" "%016x", 2u, hs(G),
                    5u + MAX)
                   && size_of(GF) == 5u + MAX
                   && says(s, &c, st(3, SFTP_FX_OK), "09%08x%s" "00000001" "%016x", 3u, hs(G),
                           5u)
                   && file_is(GF, "abcde", 5),
               "grow: SETSTAT growing by exactly SFTP_GROW_MAX is done; shrinking back too");
    test_check(says(s, &c, hd(4, 0, 1), "%s",
                    op_open_hex(4, G, SFTP_OPEN_READ | SFTP_OPEN_WRITE)),
               "grow: the file is opened for reading and writing");
    test_check(says(s, &c, st(5, SFTP_FX_FAILURE), "0a%08x%s" "00000001" "%016x", 5u,
                    hh(0, 1), 5u + MAX + 1u)
                   && size_of(GF) == 5u
                   && says(s, &c, st(6, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 6u,
                           hh(0, 1), 5u + MAX)
                   && size_of(GF) == 5u + MAX
                   && says(s, &c, st(7, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 7u,
                           hh(0, 1), 5u),
               "grow: FSETSTAT likewise: SFTP_GROW_MAX + 1 refused, exactly it done");
    /* A shrink whose new size is itself past SFTP_GROW_MAX takes the path
       that asks the file its size: it must still be a shrink. */
    test_check(says(s, &c, st(31, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 31u,
                    hh(0, 1), 5u + MAX)
                   && says(s, &c, st(32, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 32u,
                           hh(0, 1), MAX + 1u)
                   && size_of(GF) == MAX + 1u
                   && says(s, &c, st(33, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 33u,
                           hh(0, 1), 5u)
                   && size_of(GF) == 5u,
               "grow: FSETSTAT shrinking from above SFTP_GROW_MAX to above it is done");
    test_check(says(s, &c, st(8, SFTP_FX_FAILURE), "06%08x%s%016x%s", 8u, hh(0, 1),
                    5u + MAX + 1u, hs("x"))
                   && size_of(GF) == 5u
                   && says(s, &c, st(9, SFTP_FX_OK), "06%08x%s%016x%s", 9u, hh(0, 1),
                           5u + MAX, hs("x"))
                   && size_of(GF) == 5u + MAX + 1u
                   && says(s, &c, st(10, SFTP_FX_OK), "0a%08x%s" "00000001" "%016x", 10u,
                           hh(0, 1), 5u),
               "grow: a WRITE leaving a gap of SFTP_GROW_MAX + 1 is FAILURE, of exactly it "
               "done");
    test_check(says(s, &c, st(11, SFTP_FX_EOF), "05%08x%s%016x" "00000010", 11u, hh(0, 1),
                    5u + MAX + 1u)
                   && says(s, &c, st(12, SFTP_FX_FAILURE), "06%08x%s%016x%s", 12u, hh(0, 1),
                           5u + MAX + 1u, hs("x"))
                   && size_of(GF) == 5u,
               "grow: a READ past the end, then a WRITE at the same offset, is still FAILURE");
    test_check(says(s, &c, st(13, SFTP_FX_OK), "06%08x%s%016x%s", 13u, hh(0, 1), 5u, hs("f"))
                   && says(s, &c, st(14, SFTP_FX_OK), "06%08x%s%016x%s", 14u, hh(0, 1), 6u,
                           hs("g"))
                   && says(s, &c, st(15, SFTP_FX_OK), "04%08x%s", 15u, hh(0, 1))
                   && file_is(GF, "abcdefg", 7),
               "grow: WRITEs in order at the end are not limited");
    test_check(!c.ended, "grow: none of that ended the session");
    sftp_session_close(s);
    test_check(rm_tree(W_FS "\\gr"), "grow: the file is removed");
}

/* What the session asked of the resume hook. */
static unsigned m_resumes;     /* calls */
static void    *m_resume_ctx;  /* the last call's ctx */
static bool     m_resume_ok;   /* what it answers */

static bool
cap_resume(void *ctx)
{
    m_resumes++;
    m_resume_ctx = ctx;
    return m_resume_ok;
}

/* SETSTAT size @a size of @a path, id @a id, framed, as hex. */
static const char *
setsize_pk(uint32_t id, const char *path, uint32_t size)
{
    static char body[2][400];
    static unsigned k;
    char *b = body[k++ % 2];

    axl_snprintf(b, 400, "09%08x%s" "00000001" "%016x", (unsigned)id, hs(path),
                 (unsigned)size);
    return pk(b);
}

/* Call @a fn on @a s; everything it writes must be exactly @a want (hex;
   "" for nothing). */
static bool
call_says(void (*fn)(SftpSession *), SftpSession *s, Cap *c, const char *want)
{
    size_t from = c->len;

    fn(s);
    char *got = cap_hex(c, from);
    bool  ok  = got != NULL && axl_strcmp(got, want) == 0;
    if (!ok) {
        axl_printf("    got:  [%s]\n    want: [%s]\n", got != NULL ? got : "(oom)", want);
    }
    axl_free(got);
    return ok;
}

/* The resume a yield asked for, as sshd's loop delivers it. */
static bool
woke_says(SftpSession *s, Cap *c, const char *want)
{
    return call_says(sftp_session_resume, s, c, want);
}

/* One zero-fill per dispatch: after a request that grew a file past its end
   the session answers it, asks the resume hook to wake it, and dispatches
   NOTHING more -- not what it holds, not what arrives in later data calls
   (AxlSsh delivers each CHANNEL_DATA in its own call, ~700 per callback) --
   until sftp_session_writable(). Shrinks and in-order WRITEs fill nothing
   and never yield; a hook that cannot arrange the resume lets it carry on. */
static void
test_session_grow_yield(void)
{
    static Cap  c;
    static char two[1200];
    char        want[200];
    const char *A  = W "/gy/a.bin";
    const char *B  = W "/gy/b.bin";

    test_check(rm_tree(W_FS "\\gy") && axl_dir_mkdir(W_FS "\\gy") == AXL_OK
                   && axl_file_set_contents(W_FS "\\gy\\a.bin", "", 0) == AXL_OK
                   && axl_file_set_contents(W_FS "\\gy\\b.bin", "", 0) == AXL_OK,
               "grow-yield: two empty files are written");
    sftp_set_resume(cap_resume);
    m_resumes   = 0;
    m_resume_ok = true;
    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        sftp_set_resume(NULL);
        test_skip_n(16, "no session");
        return;
    }
    axl_snprintf(two, sizeof(two), "%s%s", setsize_pk(1, A, 1000), setsize_pk(2, B, 1000));
    size_t first = axl_strlen(setsize_pk(1, A, 1000)) / 2u;
    size_t c0    = c.consumed;
    test_check(sess_says(s, &c, two, st(1, SFTP_FX_OK)) && size_of(W_FS "\\gy\\a.bin") == 1000
                   && size_of(W_FS "\\gy\\b.bin") == 0 && m_resumes == 1
                   && m_resume_ctx == &c,
               "grow-yield: of two pipelined growths only the first is answered, and the "
               "session asks once to be resumed");
    /* The channel's own writable can arrive in the same drain as the
       yield: it must not end it, or two zero-fills run back to back. */
    test_check(c.consumed - c0 == first,
               "grow-yield: only the answered request is reported consumed; the one the "
               "yield holds is not, so the client's window stays shut behind it");
    test_check(call_says(sftp_session_writable, s, &c, "")
                   && size_of(W_FS "\\gy\\b.bin") == 0 && m_resumes == 1,
               "grow-yield: the channel's writable does not end a yield; only the resume does");
    test_check(woke_says(s, &c, st(2, SFTP_FX_OK)) && size_of(W_FS "\\gy\\b.bin") == 1000
                   && m_resumes == 2 && woke_says(s, &c, ""),
               "grow-yield: the resume answers the second, which yields again; the next "
               "resume finds nothing to do");
    test_check(c.consumed - c0 == axl_strlen(two) / 2u,
               "grow-yield: the resume reports the held request consumed");
    /* Three growths, each in its own data call, as separate CHANNEL_DATA
       packets arrive: the yield must hold across the calls. */
    test_check(sess_says(s, &c, setsize_pk(20, A, 2000), st(20, SFTP_FX_OK))
                   && sess_says(s, &c, setsize_pk(21, A, 3000), "")
                   && sess_says(s, &c, setsize_pk(22, A, 4000), "")
                   && size_of(W_FS "\\gy\\a.bin") == 2000 && m_resumes == 3,
               "grow-yield: three growths in three data calls: only the first is answered");
    test_check(woke_says(s, &c, st(21, SFTP_FX_OK)) && size_of(W_FS "\\gy\\a.bin") == 3000
                   && woke_says(s, &c, st(22, SFTP_FX_OK))
                   && size_of(W_FS "\\gy\\a.bin") == 4000 && woke_says(s, &c, "")
                   && m_resumes == 5,
               "grow-yield: ...then one per resume");
    axl_snprintf(two, sizeof(two), "%s%s", setsize_pk(3, A, 0), setsize_pk(4, B, 0));
    axl_snprintf(want, sizeof(want), "%s%s", st(3, SFTP_FX_OK), st(4, SFTP_FX_OK));
    test_check(sess_says(s, &c, two, want) && m_resumes == 5,
               "grow-yield: shrinks fill nothing: both answered, no yield");
    test_check(says(s, &c, hd(5, 0, 1), "%s", op_open_hex(5, A, SFTP_OPEN_WRITE)),
               "grow-yield: a.bin is opened for writing");
    char three[600];
    char b1[200];
    char b2[200];
    char b3[200];
    axl_snprintf(b1, sizeof(b1), "06%08x%s%016x%s", 6u, hh(0, 1), 0u, hs("ab"));
    axl_snprintf(b2, sizeof(b2), "06%08x%s%016x%s", 7u, hh(0, 1), 2u, hs("cd"));
    axl_snprintf(b3, sizeof(b3), "04%08x%s", 8u, hh(0, 1));
    axl_snprintf(three, sizeof(three), "%s%s%s", pk(b1), pk(b2), pk(b3));
    axl_snprintf(want, sizeof(want), "%s%s%s", st(6, SFTP_FX_OK), st(7, SFTP_FX_OK),
                 st(8, SFTP_FX_OK));
    test_check(sess_says(s, &c, three, want) && m_resumes == 5
                   && file_is(W_FS "\\gy\\a.bin", "abcd", 4),
               "grow-yield: WRITEs in order at the end fill nothing: all answered, no yield");
    /* A WRITE leaving a small gap zero-fills it too: it counts. */
    test_check(says(s, &c, hd(13, 0, 2), "%s", op_open_hex(13, A, SFTP_OPEN_WRITE)),
               "grow-yield: a.bin is opened again");
    axl_snprintf(b1, sizeof(b1), "06%08x%s%016x%s", 11u, hh(0, 2), 100u, hs("z"));
    axl_snprintf(two, sizeof(two), "%s%s", pk(b1), setsize_pk(12, B, 2000));
    test_check(sess_says(s, &c, two, st(11, SFTP_FX_OK)) && m_resumes == 6
                   && size_of(W_FS "\\gy\\a.bin") == 101 && size_of(W_FS "\\gy\\b.bin") == 0,
               "grow-yield: a WRITE 96 bytes past the end counts as a growth: it yields");
    test_check(woke_says(s, &c, st(12, SFTP_FX_OK)) && size_of(W_FS "\\gy\\b.bin") == 2000
                   && woke_says(s, &c, ""),
               "grow-yield: ...and the resume answers what followed it");
    m_resume_ok = false;
    axl_snprintf(two, sizeof(two), "%s%s", setsize_pk(9, A, 3000), setsize_pk(10, B, 3000));
    axl_snprintf(want, sizeof(want), "%s%s", st(9, SFTP_FX_OK), st(10, SFTP_FX_OK));
    test_check(sess_says(s, &c, two, want) && m_resumes == 9,
               "grow-yield: when the resume cannot be arranged the session carries on");
    /* A growth that was allowed but did not happen -- the volume is full --
       filled nothing, so it must not yield: a filler leaves 1 MiB free and
       a 2 MiB growth of b.bin (3000 bytes) fails. The request behind it
       sets a.bin to the size it already has, so it changes nothing and
       cannot mask what the failure did to b.bin. A volume with too little
       room for the case is a SKIP, not a failure. */
    uint64_t freeb = 0;
    m_resume_ok    = true;
    unsigned before = m_resumes;
    if (axl_volume_get_space("fs0:", NULL, &freeb) != AXL_OK || freeb <= 4u * 1024u * 1024u) {
        test_skip_n(1, "grow-yield: too little free space on fs0 for the full-volume case");
    } else {
        bool filled = axl_file_set_contents(W_FS "\\gy\\fill.bin", "", 0) == AXL_OK
                      && axl_file_truncate(W_FS "\\gy\\fill.bin",
                                           freeb - 1024u * 1024u) == AXL_OK;
        axl_snprintf(two, sizeof(two), "%s%s",
                     setsize_pk(40, B, 3000u + 2u * 1024u * 1024u), setsize_pk(41, A, 3000));
        axl_snprintf(want, sizeof(want), "%s%s", st(40, SFTP_FX_FAILURE), st(41, SFTP_FX_OK));
        test_check(filled && size_of(W_FS "\\gy\\b.bin") == 3000
                       && size_of(W_FS "\\gy\\a.bin") == 3000
                       && sess_says(s, &c, two, want) && m_resumes == before
                       && size_of(W_FS "\\gy\\b.bin") == 3000,
                   "grow-yield: a growth that failed (volume full) left b.bin's size as it "
                   "was and filled nothing: no yield");
    }
    sftp_session_close(s);
    sftp_set_resume(NULL);
    test_check(rm_tree(W_FS "\\gy"), "grow-yield: the files are removed");
}

/* Calls of the resume callback, and the data of the last. */
static unsigned m_resume_calls;
static void    *m_resume_data;

static void
on_resume(void *data)
{
    m_resume_calls++;
    m_resume_data = data;
}

/* Run @a loop's next tick without waiting: the defers it holds run. */
static void
tick(AxlLoop *loop)
{
    int passes = 0;

    while (passes < 3 && axl_loop_dispatch(loop, false) >= 0) {
        passes++;
    }
}

/* The relay's resume bookkeeping (sshd-sftp-resume.h) against a real loop:
   one deferred call per session however often it is asked for, none
   pending once it has run, and none run after a cancel -- the channel
   closing while a resume is pending must not have it reach a freed
   session. */
static void
test_resume_bookkeeping(void)
{
    AxlLoop   *loop = axl_loop_new();
    SftpResume r    = { 0 };
    int        tag  = 0;

    if (loop == NULL) {
        test_skip_n(5, "no loop");
        return;
    }
    m_resume_calls = 0;
    test_check(sftp_resume_ask(&r, loop, on_resume, &tag) && r.handle != 0
                   && sftp_resume_ask(&r, loop, on_resume, &tag) && m_resume_calls == 0,
               "resume: asked twice, one call is pending and none has run yet");
    tick(loop);
    test_check(m_resume_calls == 1 && m_resume_data == &tag,
               "resume: the next tick runs it once, with the session's data");
    sftp_resume_fired(&r);
    test_check(r.handle == 0 && sftp_resume_ask(&r, loop, on_resume, &tag) && r.handle != 0,
               "resume: once it has run, a new ask schedules a new call");
    test_check(sftp_resume_cancel(&r, loop) && r.handle == 0
                   && sftp_resume_cancel(&r, loop),
               "resume: cancel leaves nothing pending; cancelling nothing is fine");
    tick(loop);
    test_check(m_resume_calls == 1 && !sftp_resume_ask(&r, NULL, on_resume, &tag)
                   && r.handle == 0,
               "resume: a cancelled call never runs; with no loop nothing is scheduled");
    axl_loop_free(loop);
}

/* A file deleted under an open handle: FSTAT and FSETSTAT through it fail
   (the FAT driver fails every query on a handle whose file is gone), the
   handle still closes, and the session goes on. */
static void
test_session_deleted_under_handle(void)
{
    static Cap c;
    char       want[96];

    SftpSession *s = sess_new(&c, 0);
    if (s == NULL) {
        test_skip_n(3, "no session");
        return;
    }
    test_check(axl_file_set_contents(W_FS "\\gone.txt", "x", 1) == AXL_OK
                   && says(s, &c, hd(1, 0, 1), "%s",
                           op_open_hex(1, W "/gone.txt", SFTP_OPEN_WRITE))
                   && axl_file_delete(W_FS "\\gone.txt") == AXL_OK,
               "deleted: a file opened for writing, then deleted by path");
    axl_snprintf(want, sizeof(want), "0000001d69%08x0000000d0000000000000000000041ed"
                 "0000000000000000", 5u);
    test_check(says(s, &c, st(2, SFTP_FX_FAILURE), "08%08x%s", 2u, hh(0, 1))
                   && says(s, &c, st(3, SFTP_FX_FAILURE), "0a%08x%s" "00000001" "%016x", 3u,
                           hh(0, 1), 0u)
                   && says(s, &c, st(4, SFTP_FX_OK), "04%08x%s", 4u, hh(0, 1))
                   && says(s, &c, want, "11%08x%s", 5u, hs("/")),
               "deleted: FSTAT and FSETSTAT are FAILURE, CLOSE is OK, a STAT is answered");
    test_check(!c.ended && !exists(W_FS "\\gone.txt"), "deleted: the session lives");
    sftp_session_close(s);
}

/* The write tests' fixtures go, and with them their directory. */
static void
test_session_write_cleanup(void)
{
    test_check(axl_file_delete(W_FS "\\ro.txt") == AXL_OK
                   && axl_file_delete(W_FS "\\new.txt") == AXL_OK
                   && axl_dir_rmdir(W_FS) == AXL_OK && !exists(W_FS),
               "write tests: the fixture directory is removed");
}

static void
test_session_handle_cap(void)
{
    static Cap c;
    char       h[512];
    bool       all = true;
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(2, "no session");
        return;
    }
    for (uint32_t i = 0; i < 16; i++) {
        axl_snprintf(h, sizeof(h), "03" "%08x" S_FIXTURE "00000001" "00000000",
                     (unsigned)(200 + i));
        all = sess_says(s, &c, pk(h), hd(200 + i, i, 1)) && all;
    }
    test_check(all, "session: 16 opens get slots 0-15");
    axl_snprintf(h, sizeof(h), "03" "%08x" S_FIXTURE "00000001" "00000000", 216u);
    test_check(sess_says(s, &c, pk(h), st(216, SFTP_FX_FAILURE)),
               "session: the 17th open is FAILURE (sftp-server's handle_new refusal)");
    /* Closing the session with 16 files open releases them all: the leak
       verdict at the end of the run says so. */
    sftp_session_close(s);
}

/* Packets that end a session, as sftp-server exits on them: before INIT
   (255), a length over 256 KB (11), a request cut short (255). A zero
   length ends it too, with 11 here: sftp-server does not keep serving
   either -- it reads the next packet's first byte as this one's type and
   exits 255 -- but 11, its bad-length status, says what was wrong. Nothing
   is written for any of them. */
static void
test_session_fatal(void)
{
    static Cap c;
    struct {
        const char *in;
        uint32_t    status;
        const char *name;
    } F[] = {
        { "0000000911" "00000001" "00000000", 255,
          "session fatal: a request before INIT ends it, 255, nothing written" },
        { "0000000000", 11, "session fatal: a zero length ends it, 11" },
        { "00040001" "11", 11,
          "session fatal: a length over 256 KB ends it, 11, before the body arrives" },
        { "0000000911" "00000001" "00000032", 255,
          "session fatal: a path length past the packet ends it, 255" },
        /* sftp-server's process_extended_posix_rename() is fatal on a
           missing argument, as on any short field. */
        { "0000002b" "c8" "00000001" "00000018"
          "706f7369782d72656e616d65406f70656e7373682e636f6d" "00000006" "2f6673302f61",
          255, "session fatal: a posix-rename with one path ends it, 255" },
    };

    for (size_t i = 0; i < AXL_ARRAY_SIZE(F); i++) {
        static uint8_t b[64];
        axl_memset(&c, 0, sizeof(c));
        SftpSession *s = sftp_session_open(cap_write, cap_end, cap_consumed, &c, 0, 8192);
        if (s == NULL) {
            test_check(false, F[i].name);
            continue;
        }
        if (i > 0) {
            uint8_t init[9];
            sftp_session_data(s, init, unhex(pk(S_INIT), init, sizeof(init)));
        }
        size_t from = c.len;
        size_t n    = unhex(F[i].in, b, sizeof(b));
        sftp_session_data(s, b, n);
        test_check(c.ended && c.status == F[i].status && c.len == from, F[i].name);
        sftp_session_close(s);
    }
}

/* Hostile but survivable: an unknown type gets no reply and the next request
   is answered; trailing bytes are ignored; a request split across many
   small chunks is answered once whole; BUSY holds the reply until writable;
   EOF ends the session with 0. */
/* A session that ENDS reports nothing more: the request it dispatched
   before the bad packet is reported, the bad packet and everything after
   it are not -- sshd's relay switches the channel back to ON_ARRIVAL at
   that end (relay_end), which is what credits them; that relay side is not
   observable here. */
static void
test_session_consumed_end(void)
{
    static Cap c;
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(3, "no session");
        return;
    }
    size_t  c0 = c.consumed;
    uint8_t b[64];
    char    hx[128];
    /* An LSTAT of "/" (14 bytes framed), then a length past SFTP_PACKET_MAX. */
    axl_snprintf(hx, sizeof(hx), "%s%s", pk("11" "00000140" "00000001" "2f"),
                 "7fffffff" "11");
    size_t n = unhex(hx, b, sizeof(b));
    sftp_session_data(s, b, n);
    test_check(c.ended && c.status == 11 && c.consumed - c0 == 14,
               "session end: the request before the bad length is reported, the bad "
               "packet is not, and the session ends with 11");
    sftp_session_data(s, b, n);
    test_check(c.consumed - c0 == 14,
               "session end: data after the end is not reported either");
    sftp_session_eof(s);
    test_check(c.consumed - c0 == 14, "session end: nor is anything at EOF");
    sftp_session_close(s);
}

static void
test_session_hostile(void)
{
    static Cap c;

    test_check(sftp_session_open(cap_write, cap_end, NULL, &c, 0, 8192) == NULL,
               "session: no consumed callback, no session -- its client's window "
               "would never reopen");
    SftpSession *s = sess_new(&c, 0);

    if (s == NULL) {
        test_skip_n(11, "no session");
        return;
    }
    char want[512];
    axl_snprintf(want, sizeof(want), "%s", st(301, SFTP_FX_NO_SUCH_FILE));
    test_check(sess_says(s, &c,
                         "0000000563" "0000012c"            /* type 99 */
                         "0000000f11" "0000012d" "00000006" "2f6e6f706521",
                         want),
               "session hostile: an unknown type is not answered; the next request is");
    /* Paths too long for the volume: BAD_MESSAGE, as sftp-server answers
       ENAMETOOLONG, for STAT, REALPATH, OPEN and OPENDIR; one byte shorter is
       an ordinary miss. The session lives. */
    static const char *const LONG_OPS[] = { "11", "10", "03", "0b" };
    bool long_ok = true;
    for (uint32_t len = SFTP_PATH_MAX - 1u; len <= SFTP_PATH_MAX + 976u; len += 977u) {
        for (size_t i = 0; i < AXL_ARRAY_SIZE(LONG_OPS); i++) {
            static char h[6000];
            size_t k = (size_t)axl_snprintf(h, sizeof(h), "%s" "00000200" "%08x" "2f",
                                            LONG_OPS[i], (unsigned)len);
            for (uint32_t j = 1; j < len; j++) {
                h[k++] = '6';
                h[k++] = '1';
            }
            if (i == 2) {
                k += (size_t)axl_snprintf(h + k, sizeof(h) - k, "0000000100000000");
            }
            h[k] = '\0';
            uint32_t want = len >= SFTP_PATH_MAX ? SFTP_FX_BAD_MESSAGE : SFTP_FX_NO_SUCH_FILE;
            long_ok = sess_says(s, &c, pk(h), st(0x200, want)) && long_ok;
        }
    }
    {
        /* "fs0:" and 1019 bytes: 1023 on the wire, 1024 as /fs0/... */
        static char h[2200];
        size_t k = (size_t)axl_snprintf(h, sizeof(h), "11" "00000201" "%08x" "6673303a",
                                        (unsigned)(SFTP_PATH_MAX - 1u));
        for (uint32_t j = 4; j < SFTP_PATH_MAX - 1u; j++) {
            h[k++] = '6';
            h[k++] = '1';
        }
        h[k] = '\0';
        test_check(sess_says(s, &c, pk(h), st(0x201, SFTP_FX_BAD_MESSAGE)),
                   "session hostile: a 1023-byte fs0: path, too long once normalised, is BAD_MESSAGE");
    }
    test_check(long_ok && !c.ended,
               "session hostile: a path of 1024+ bytes is BAD_MESSAGE (STAT, REALPATH, OPEN, "
               "OPENDIR); 1023 is a miss; the session lives");
    test_check(sess_says(s, &c, pk("11" "0000012e" "00000001" "2f" "deadbeef"),
                         "0000001d69" "0000012e" "0000000d" "0000000000000000"
                         "000041ed" "00000000" "00000000"),
               "session hostile: trailing bytes after a request are ignored");
    /* One request fed a byte at a time. */
    uint8_t one[64];
    size_t  n    = unhex(pk("11" "00000130" "00000001" "2f"), one, sizeof(one));
    size_t  from = c.len;
    for (size_t i = 0; i < n; i++) {
        sftp_session_data(s, one + i, 1);
    }
    char *got = cap_hex(&c, from);
    test_check(got != NULL
                   && axl_strcmp(got, "0000001d69" "00000130" "0000000d" "0000000000000000"
                                      "000041ed" "00000000" "00000000") == 0,
               "session hostile: a request in 1-byte chunks is answered once, whole");
    axl_free(got);
    /* BUSY: the reply waits for writable, then leaves whole -- and the
       request queued behind it is not reported consumed until it is
       answered, so the client's window stays shut while the channel is
       behind. */
    c.busy = 1;
    size_t c1  = c.consumed;
    char   two[64];
    axl_snprintf(two, sizeof(two), "%s%s", pk("11" "00000131" "00000001" "2f"),
                 pk("11" "00000132" "00000001" "2f"));
    test_check(sess_says(s, &c, two, ""),
               "session hostile: a refused write holds the reply");
    test_check(c.consumed - c1 == 14,
               "session hostile: behind BUSY, only the request answered is reported "
               "consumed (4 + 10 bytes)");
    from = c.len;
    sftp_session_writable(s);
    got = cap_hex(&c, from);
    test_check(got != NULL
                   && axl_strcmp(got, "0000001d69" "00000131" "0000000d" "0000000000000000"
                                      "000041ed" "00000000" "00000000"
                                      "0000001d69" "00000132" "0000000d" "0000000000000000"
                                      "000041ed" "00000000" "00000000") == 0
                   && !c.ended,
               "session hostile: writable sends the held reply, then answers the request "
               "queued behind it");
    axl_free(got);
    test_check(c.consumed - c1 == 28 && c.len > from,
               "session hostile: writable answers the request held behind it and "
               "reports it consumed");
    sftp_session_eof(s);
    test_check(c.ended && c.status == 0, "session: the client's EOF ends it with 0");
    sftp_session_close(s);
    test_check(axl_file_delete(FIXTURE_FS) == AXL_OK, "session: the fixture is removed");
}

static int
test_sshd_sftp_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("SshdSftp");

    test_rx_chunks();
    test_rx_two_in_one();
    test_rx_bounds();
    test_decode_types();
    test_decode_attrs();
    test_decode_truncated();
    test_decode_bounds();
    test_encode();
    test_status_messages();
    test_attrs_synth();
    test_longname();
    test_handles();
    test_paths();
    test_session_basics();
    test_request_classes();
    test_extensions();
    test_session_read_only();
    test_session_open_write();
    test_session_setstat();
    test_session_namespace();
    test_session_cap_creates_nothing();
    test_session_ro_attr();
    test_session_posix_rename();
    test_session_grow();
    test_session_grow_yield();
    test_resume_bookkeeping();
    test_session_handle_follows_file();
    test_session_times();
    test_session_deleted_under_handle();
    test_session_write_cleanup();
    test_session_handle_cap();
    test_session_fatal();
    test_session_hostile();
    test_session_consumed_end();

    return test_print_results();
}

AXL_APP(test_sshd_sftp_main)
