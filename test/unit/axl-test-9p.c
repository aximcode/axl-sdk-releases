/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

#include <axl.h>
#include "axl-test.h"                 /* test_check / test_print_results */
#include <axl/axl-str.h>              /* axl_strcmp */
#include "../../src/9p/axl-9p-internal.h"
#include "../../src/9p/axl-9p-server-internal.h"
#include "axl-test-flushfail-fs.h"

static void
test_codec_roundtrip(void)
{
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    axl_9p_w_u8(&w, 0x12);
    axl_9p_w_u16(&w, 0x3456);
    axl_9p_w_u32(&w, 0x89abcdefu);
    axl_9p_w_u64(&w, 0x0011223344556677ull);
    axl_9p_w_str(&w, "hello");
    test_check(!w.overflow, "9p codec: writer did not overflow");

    Axl9pReader r;
    axl_9p_r_init(&r, buf, w.len);
    test_check(axl_9p_r_u8(&r)  == 0x12,             "9p codec: u8 roundtrip");
    test_check(axl_9p_r_u16(&r) == 0x3456,           "9p codec: u16 roundtrip");
    test_check(axl_9p_r_u32(&r) == 0x89abcdefu,      "9p codec: u32 roundtrip");
    test_check(axl_9p_r_u64(&r) == 0x0011223344556677ull, "9p codec: u64 roundtrip");
    char s[16];
    size_t n = axl_9p_r_str(&r, s, sizeof(s));
    test_check(n == 5 && axl_strcmp(s, "hello") == 0, "9p codec: str roundtrip");
    test_check(!r.error, "9p codec: reader did not underrun");
}

static void
test_codec_byte_layout(void)
{
    /* Round-trip tests alone can't catch an endianness flip (a
     * byte-swapped write followed by a byte-swapped read still
     * round-trips). Pin the raw on-wire bytes directly. */
    uint8_t buf[8];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    axl_9p_w_u16(&w, 0x3456);
    axl_9p_w_u32(&w, 0x89abcdefu);
    test_check(!w.overflow, "9p codec: byte-layout writer did not overflow");

    test_check(buf[0] == 0x56 && buf[1] == 0x34,
               "9p codec: u16 little-endian byte order");
    test_check(buf[2] == 0xef && buf[3] == 0xcd
             && buf[4] == 0xab && buf[5] == 0x89,
               "9p codec: u32 little-endian byte order");
}

static void
test_codec_str_truncation_boundary(void)
{
    /* cap==1 leaves room for the NUL terminator only — out[0] must be
     * '\0' and the full on-wire length must still be returned/consumed,
     * even though nothing of the string itself fits. */
    uint8_t buf[32];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    axl_9p_w_str(&w, "hello");
    test_check(!w.overflow, "9p codec: str-truncation writer did not overflow");

    Axl9pReader r;
    axl_9p_r_init(&r, buf, w.len);
    char s[1];
    size_t n = axl_9p_r_str(&r, s, sizeof(s));
    test_check(n == 5, "9p codec: str truncation returns full wire length");
    test_check(s[0] == '\0', "9p codec: str truncation with cap==1 yields empty string");
    test_check(!r.error, "9p codec: str truncation does not flag reader error");
}

static void
test_codec_overflow_and_underrun(void)
{
    uint8_t small[2];
    Axl9pWriter w;
    axl_9p_w_init(&w, small, sizeof(small));
    axl_9p_w_u32(&w, 0xdeadbeefu);          /* 4 bytes into a 2-byte buffer */
    test_check(w.overflow, "9p codec: overflow flagged on short buffer");

    uint8_t one[1] = { 0xAB };
    Axl9pReader r;
    axl_9p_r_init(&r, one, sizeof(one));
    (void)axl_9p_r_u32(&r);                 /* reads past end */
    test_check(r.error, "9p codec: underrun flagged on short read");
}

static void
test_9p_w_bytes(void)
{
    uint8_t buf[8];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    const uint8_t payload[3] = { 0xDE, 0xAD, 0xBE };
    axl_9p_w_u8(&w, 0x11);
    axl_9p_w_bytes(&w, payload, sizeof(payload));
    test_check(!w.overflow, "w_bytes: no overflow within cap");
    test_check(w.len == 4, "w_bytes: len advanced by 1 + 3");
    test_check(buf[0] == 0x11 && buf[1] == 0xDE && buf[2] == 0xAD && buf[3] == 0xBE,
               "w_bytes: exact bytes appended after the u8");

    /* Overflow: 6 more bytes into a 4-remaining buffer must set overflow and
       not write past cap. */
    axl_9p_w_bytes(&w, payload, 3);            /* fits: len -> 7 */
    axl_9p_w_bytes(&w, payload, 3);            /* 3 into 1 remaining -> overflow */
    test_check(w.overflow, "w_bytes: overflow flagged when data exceeds cap");
}

static void
test_tversion_golden(void)
{
    /* Tversion{ tag=0xffff, msize=8192, version="9P2000.L" } has a fixed
       21-byte wire encoding: size[4]+type[1]+tag[2]+msize[4]+strlen[2]+
       "9P2000.L"[8] = 7+4+2+8 = 21. (The commonly-cited "19-byte" Tversion
       example uses the 6-char base "9P2000" string, not the 8-char
       "9P2000.L" this SDK targets — 7+4+2+6 = 19.) Pin it exactly
       (endianness + field order). */
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_TVERSION, 0xFFFF);
    axl_9p_w_u32(&w, AXL_9P_MSIZE);
    axl_9p_w_str(&w, "9P2000.L");
    size_t total = axl_9p_msg_finish(&w);

    static const uint8_t want[] = {
        0x15, 0x00, 0x00, 0x00,           /* size = 21 */
        0x64,                             /* type = Tversion (100) */
        0xFF, 0xFF,                       /* tag  = 0xffff */
        0x00, 0x20, 0x00, 0x00,           /* msize = 8192 */
        0x08, 0x00,                       /* strlen = 8 */
        '9','P','2','0','0','0','.','L'
    };
    test_check(total == sizeof(want), "9p frame: Tversion length is 21");
    test_check(axl_memcmp(buf, want, sizeof(want)) == 0,
               "9p frame: Tversion golden bytes match");

    /* Header parse round-trips. */
    Axl9pReader r;
    axl_9p_r_init(&r, buf, total);
    uint32_t sz; uint8_t ty; uint16_t tag;
    test_check(axl_9p_msg_header(&r, &sz, &ty, &tag),
               "9p frame: header parses");
    test_check(sz == 21 && ty == AXL_9P_TVERSION && tag == 0xFFFF,
               "9p frame: header fields correct");
}

// Tgetattr: fid[4] request_mask[8]. Rgetattr (9P2000.L): valid[8] qid[13]
// mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8]
// atime_sec[8] atime_nsec[8] mtime_sec[8] ... (we only need mode/size/mtime_sec).
#define P9_GETATTR_BASIC  0x000007ffULL

static void
test_9p_getattr_codec(void)
{
    // --- encode Tgetattr(fid=5, mask=BASIC) ---
    uint8_t req[32];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TGETATTR, 7);
    axl_9p_w_u32(&w, 5);
    axl_9p_w_u64(&w, P9_GETATTR_BASIC);
    size_t n = axl_9p_msg_finish(&w);
    test_check(!w.overflow, "getattr: encode did not overflow");
    // size[4] type[1] tag[2] fid[4] mask[8] = 19 bytes
    test_check(n == 19, "getattr: Tgetattr encoded length is 19");
    test_check(req[4] == AXL_9P_TGETATTR, "getattr: type byte is TGETATTR(24)");

    // --- decode a synthetic Rgetattr body (mode=S_IFDIR, size=4096, mtime=111) ---
    uint8_t body[160];
    Axl9pWriter bw;
    axl_9p_w_init(&bw, body, sizeof(body));
    axl_9p_w_u64(&bw, P9_GETATTR_BASIC);          // valid
    for (int i = 0; i < 13; i++) axl_9p_w_u8(&bw, 0);  // qid[13]
    axl_9p_w_u32(&bw, 0040000u);                  // mode = S_IFDIR
    axl_9p_w_u32(&bw, 0);                          // uid
    axl_9p_w_u32(&bw, 0);                          // gid
    axl_9p_w_u64(&bw, 1);                          // nlink
    axl_9p_w_u64(&bw, 0);                          // rdev
    axl_9p_w_u64(&bw, 4096);                       // size
    axl_9p_w_u64(&bw, 512);                        // blksize
    axl_9p_w_u64(&bw, 8);                          // blocks
    axl_9p_w_u64(&bw, 0);                          // atime_sec
    axl_9p_w_u64(&bw, 0);                          // atime_nsec
    axl_9p_w_u64(&bw, 111);                        // mtime_sec

    Axl9pReader r;
    axl_9p_r_init(&r, body, bw.len);
    uint64_t valid = axl_9p_r_u64(&r);
    for (int i = 0; i < 13; i++) axl_9p_r_u8(&r);  // skip qid
    uint32_t mode  = axl_9p_r_u32(&r);
    axl_9p_r_u32(&r);                              // uid
    axl_9p_r_u32(&r);                              // gid
    axl_9p_r_u64(&r);                              // nlink
    axl_9p_r_u64(&r);                              // rdev
    uint64_t size = axl_9p_r_u64(&r);
    axl_9p_r_u64(&r);                              // blksize
    axl_9p_r_u64(&r);                              // blocks
    axl_9p_r_u64(&r);                              // atime_sec
    axl_9p_r_u64(&r);                              // atime_nsec
    uint64_t mtime = axl_9p_r_u64(&r);
    test_check(!r.error, "getattr: Rgetattr decode did not underrun");
    test_check(valid == P9_GETATTR_BASIC, "getattr: valid mask round-trips");
    test_check((mode & 0040000u) != 0, "getattr: S_IFDIR bit decoded");
    test_check(size == 4096, "getattr: size decoded == 4096");
    test_check(mtime == 111, "getattr: mtime_sec decoded == 111");
}

/* Synthetic dirent spec for building fake Rreaddir bodies below -- one
   entry per (offset, dtype, name) triple. */
typedef struct {
    uint64_t    off;
    uint8_t     dtype;
    const char *name;
} DirEntSpec;

/* Build a well-formed Rreaddir reply body: dcount[4] followed by `n`
   synthetic dirents (qid[13] offset[8] type[1] name[s]). The qid bytes
   are arbitrary -- axl_9p_parse_readdir skips them unread. dcount is
   computed to exactly match the encoded entry bytes (a truthful reply).
   Returns the total body length written (== 4 + entries bytes). */
static size_t
build_readdir_body(uint8_t *buf, size_t cap, const DirEntSpec *ents, size_t n)
{
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, cap);
    size_t dcount_pos = w.len;
    axl_9p_w_u32(&w, 0);                     /* dcount placeholder */
    size_t entries_start = w.len;
    for (size_t i = 0; i < n; i++) {
        axl_9p_w_u8(&w, 0);                  /* qid.type (unread) */
        axl_9p_w_u32(&w, 0);                 /* qid.version (unread) */
        axl_9p_w_u64(&w, (uint64_t)(i + 1)); /* qid.path (unread) */
        axl_9p_w_u64(&w, ents[i].off);
        axl_9p_w_u8(&w, ents[i].dtype);
        axl_9p_w_str(&w, ents[i].name);
    }
    uint32_t dcount = (uint32_t)(w.len - entries_start);
    buf[dcount_pos + 0] = (uint8_t)(dcount);
    buf[dcount_pos + 1] = (uint8_t)(dcount >> 8);
    buf[dcount_pos + 2] = (uint8_t)(dcount >> 16);
    buf[dcount_pos + 3] = (uint8_t)(dcount >> 24);
    return w.len;
}

static void
test_parse_readdir_well_formed(void)
{
    static const DirEntSpec ents[] = {
        { .off = 1, .dtype = 4, .name = "."     },   /* DT_DIR, skipped */
        { .off = 2, .dtype = 8, .name = "a.txt" },   /* DT_REG */
        { .off = 3, .dtype = 8, .name = "b.txt" },   /* DT_REG */
    };
    uint8_t buf[128];
    size_t  body_len = build_readdir_body(buf, sizeof(buf), ents, 3);

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint32_t  dcount = 0;
    uint64_t  last_off = 0;
    int rc = axl_9p_parse_readdir(buf, body_len, arr, &dcount, &last_off);

    test_check(rc == AXL_OK, "9p parse_readdir: well-formed body returns AXL_OK");
    test_check(dcount > 0, "9p parse_readdir: well-formed body reports dcount > 0");
    test_check(axl_array_len(arr) == 2,
               "9p parse_readdir: '.' is skipped, 2 real entries appended");

    AxlFsEntry *e0 = (AxlFsEntry *)axl_array_get(arr, 0);
    AxlFsEntry *e1 = (AxlFsEntry *)axl_array_get(arr, 1);
    test_check(e0 != NULL && axl_strcmp(e0->name, "a.txt") == 0,
               "9p parse_readdir: entry 0 name is a.txt");
    test_check(e1 != NULL && axl_strcmp(e1->name, "b.txt") == 0,
               "9p parse_readdir: entry 1 name is b.txt");
    test_check(e0 != NULL && (e0->attributes & AXL_FS_ATTR_DIRECTORY) == 0,
               "9p parse_readdir: DT_REG entry does not set AXL_FS_ATTR_DIRECTORY");

    axl_array_free(arr);
}

static void
test_parse_readdir_dir_entry(void)
{
    static const DirEntSpec ents[] = {
        { .off = 1, .dtype = 4, .name = "subdir" },   /* DT_DIR */
    };
    uint8_t buf[64];
    size_t  body_len = build_readdir_body(buf, sizeof(buf), ents, 1);

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint32_t  dcount = 0;
    uint64_t  last_off = 0;
    int rc = axl_9p_parse_readdir(buf, body_len, arr, &dcount, &last_off);

    test_check(rc == AXL_OK, "9p parse_readdir: DT_DIR body returns AXL_OK");
    test_check(axl_array_len(arr) == 1, "9p parse_readdir: DT_DIR body appends 1 entry");

    AxlFsEntry *e0 = (AxlFsEntry *)axl_array_get(arr, 0);
    test_check(e0 != NULL && (e0->attributes & AXL_FS_ATTR_DIRECTORY) != 0,
               "9p parse_readdir: DT_DIR(4) sets AXL_FS_ATTR_DIRECTORY");

    axl_array_free(arr);
}

static void
test_parse_readdir_eof(void)
{
    uint8_t buf[16];
    size_t  body_len = build_readdir_body(buf, sizeof(buf), NULL, 0);

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint32_t  dcount = 0xFFFFFFFFu;          /* poison -- must be overwritten with 0 */
    uint64_t  last_off = 0;
    int rc = axl_9p_parse_readdir(buf, body_len, arr, &dcount, &last_off);

    test_check(rc == AXL_OK, "9p parse_readdir: zero-entry body returns AXL_OK");
    test_check(dcount == 0, "9p parse_readdir: zero-entry body reports dcount == 0 (EOF)");
    test_check(axl_array_len(arr) == 0, "9p parse_readdir: EOF body appends no entries");

    axl_array_free(arr);
}

static void
test_parse_readdir_dcount_lies(void)
{
    /* DoS regression: a server that claims a huge dcount but supplies far
       fewer real bytes must be rejected as AXL_ERR, never treated as EOF
       -- EOF would leave the caller's `offset` unchanged and replay the
       same bad request against the same offset forever (axl_9p_list hang,
       see the fix in src/9p/axl-9p-client.c). */
    static const DirEntSpec ents[] = {
        { .off = 1, .dtype = 8, .name = "a.txt" },
    };
    uint8_t buf[64];
    size_t  body_len = build_readdir_body(buf, sizeof(buf), ents, 1);

    /* Corrupt the dcount field: claim far more entry bytes than the
       buffer actually holds after it. */
    uint32_t lie = (uint32_t)body_len + 1000u;
    buf[0] = (uint8_t)(lie);
    buf[1] = (uint8_t)(lie >> 8);
    buf[2] = (uint8_t)(lie >> 16);
    buf[3] = (uint8_t)(lie >> 24);

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint32_t  dcount = 0;
    uint64_t  last_off = 0;
    int rc = axl_9p_parse_readdir(buf, body_len, arr, &dcount, &last_off);

    test_check(rc == AXL_ERR, "9p parse_readdir: lying dcount returns AXL_ERR (DoS fix)");
    test_check(axl_array_len(arr) == 0,
               "9p parse_readdir: lying dcount appends zero entries");

    axl_array_free(arr);
}

static void
test_parse_readdir_truncated_entry(void)
{
    /* One entry whose qid[13] offset[8] type[1] are present and whose
       name-length prefix claims 200 bytes, but only 3 real bytes follow.
       dcount is set to exactly the bytes actually present (a truthful
       dcount), so the outer dcount-vs-body_len check passes -- the
       guarded axl_9p_r_str underrun inside the entry must be what catches
       this, not the outer check. */
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    size_t dcount_pos = w.len;
    axl_9p_w_u32(&w, 0);              /* dcount placeholder */
    size_t entries_start = w.len;
    axl_9p_w_u8(&w, 0);               /* qid.type (unread) */
    axl_9p_w_u32(&w, 0);              /* qid.version (unread) */
    axl_9p_w_u64(&w, 42);             /* qid.path (unread) */
    axl_9p_w_u64(&w, 1);              /* dirent offset cursor */
    axl_9p_w_u8(&w, 8);               /* DT_REG */
    axl_9p_w_u16(&w, 200);            /* claims a 200-byte name... */
    axl_9p_w_u8(&w, 'a');             /* ...but only 3 bytes follow */
    axl_9p_w_u8(&w, 'b');
    axl_9p_w_u8(&w, 'c');
    uint32_t dcount = (uint32_t)(w.len - entries_start);
    buf[dcount_pos + 0] = (uint8_t)(dcount);
    buf[dcount_pos + 1] = (uint8_t)(dcount >> 8);
    buf[dcount_pos + 2] = (uint8_t)(dcount >> 16);
    buf[dcount_pos + 3] = (uint8_t)(dcount >> 24);
    test_check(!w.overflow,
               "9p parse_readdir: truncated-entry fixture writer did not overflow");

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint32_t  out_dcount = 0;
    uint64_t  last_off = 0;
    int rc = axl_9p_parse_readdir(buf, w.len, arr, &out_dcount, &last_off);

    test_check(rc == AXL_ERR,
               "9p parse_readdir: truncated name length returns AXL_ERR, no OOB");
    test_check(axl_array_len(arr) == 0,
               "9p parse_readdir: truncated entry appends no bogus entry");

    axl_array_free(arr);
}

/* C++ RAII autoptr — AXL_AUTOPTR(Axl9pClient) registers axl_9p_disconnect as
   the scope-exit teardown. A live session needs a 9P server (exercised by the
   9p integration test), so here we pin the compile-time registration and the
   NULL-safe scope-exit path: an always-initialized NULL autoptr must tear down
   cleanly without allocating, freeing, or corrupting the allocator. */
static void
test_autoptr_9p_client(void)
{
    AxlMemStats before, after;
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(Axl9pClient) c = NULL;   /* always initialized (GLib #1595) */
        (void)c;
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "autoptr: 9p client NULL scope-exit is a clean no-op");
}

static void
test_9p_server_reply_codec(void)
{
    // Rlerror(tag=3, ecode=2 ENOENT): body = ecode[4]
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_RLERROR, 3);
    axl_9p_w_u32(&w, 2);
    size_t n = axl_9p_msg_finish(&w);
    test_check(n == 11, "server-codec: Rlerror length is 11 (hdr7 + ecode4)");
    Axl9pReader r;
    uint32_t sz; uint8_t ty; uint16_t tag;
    axl_9p_r_init(&r, buf, n);
    test_check(axl_9p_msg_header(&r, &sz, &ty, &tag), "server-codec: Rlerror header parses");
    test_check(ty == AXL_9P_RLERROR && tag == 3, "server-codec: Rlerror type+tag");
    test_check(axl_9p_r_u32(&r) == 2, "server-codec: Rlerror ecode == ENOENT");

    // Rversion(tag, msize=8192, "9P2000.L")
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_RVERSION, 0xffff);
    axl_9p_w_u32(&w, 8192);
    axl_9p_w_str(&w, "9P2000.L");
    n = axl_9p_msg_finish(&w);
    axl_9p_r_init(&r, buf, n);
    axl_9p_msg_header(&r, &sz, &ty, &tag);
    test_check(ty == AXL_9P_RVERSION, "server-codec: Rversion type");
    test_check(axl_9p_r_u32(&r) == 8192, "server-codec: Rversion msize round-trips");
    char ver[16];
    test_check(axl_9p_r_str(&r, ver, sizeof(ver)) == 8
               && axl_strcmp(ver, "9P2000.L") == 0, "server-codec: Rversion version string");

    // Rwalk(tag, nwqid=2, two qids of 13 bytes each)
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_RWALK, 1);
    axl_9p_w_u16(&w, 2);
    for (int q = 0; q < 2; q++) { for (unsigned b = 0; b < AXL_9P_QID_LEN; b++) axl_9p_w_u8(&w, (uint8_t)(q*13+b)); }
    n = axl_9p_msg_finish(&w);
    test_check(n == 7 + 2 + 2 * AXL_9P_QID_LEN, "server-codec: Rwalk length = hdr + nwqid + 2*qid");
    axl_9p_r_init(&r, buf, n);
    axl_9p_msg_header(&r, &sz, &ty, &tag);
    test_check(axl_9p_r_u16(&r) == 2, "server-codec: Rwalk nwqid == 2");
}

/* axl_9p_w_patch_u16 -- back-patch a count field reserved BEFORE the items it
   counts (Rwalk's nwqid, Twalk's nwname). Lets an encoder stream items
   straight into the wire buffer instead of accumulating them in a
   fixed-size scratch array just to learn the count up front. */
static void
test_9p_w_patch_u16(void)
{
    uint8_t buf[32];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_RWALK, 7);
    size_t count_pos = w.len;
    axl_9p_w_u16(&w, 0);          /* placeholder */
    axl_9p_w_u8(&w, 0xAA);        /* an "item" written after the placeholder */
    axl_9p_w_patch_u16(&w, count_pos, 0x0102);
    test_check(!w.overflow, "9p codec: patch_u16 in range does not set overflow");
    test_check(buf[count_pos] == 0x02 && buf[count_pos + 1] == 0x01,
               "9p codec: patch_u16 writes little-endian at the reserved offset");
    test_check(buf[count_pos + 2] == 0xAA,
               "9p codec: patch_u16 leaves following bytes untouched");
    test_check(w.len == count_pos + 3,
               "9p codec: patch_u16 does not advance the write cursor");

    /* A patch past what has actually been written must be refused, not a
       silent scribble into uninitialized buffer space. */
    Axl9pWriter w2;
    axl_9p_w_init(&w2, buf, sizeof(buf));
    axl_9p_w_u8(&w2, 1);
    axl_9p_w_patch_u16(&w2, 0, 0x1234);
    test_check(w2.overflow, "9p codec: patch_u16 past the write cursor sets overflow");
    test_check(buf[0] == 1, "9p codec: refused patch_u16 wrote nothing");
}

/* axl_9p_w_patch_u32 -- the u32 sibling of patch_u16, for the byte-count
   field that precedes a variable-length payload (Rread's count, Rreaddir's
   dcount): the encoder can't know the count until it has produced the
   payload, and staging that payload elsewhere just to count it would mean a
   second msize-sized buffer. */
static void
test_9p_w_patch_u32(void)
{
    uint8_t buf[32];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_RREAD, 9);
    size_t count_pos = w.len;
    axl_9p_w_u32(&w, 0);          /* placeholder */
    axl_9p_w_u8(&w, 0xBB);        /* a payload byte after the placeholder */
    axl_9p_w_patch_u32(&w, count_pos, 0x01020304u);
    test_check(!w.overflow, "9p codec: patch_u32 in range does not set overflow");
    test_check(buf[count_pos] == 0x04 && buf[count_pos + 1] == 0x03
               && buf[count_pos + 2] == 0x02 && buf[count_pos + 3] == 0x01,
               "9p codec: patch_u32 writes little-endian at the reserved offset");
    test_check(buf[count_pos + 4] == 0xBB,
               "9p codec: patch_u32 leaves following bytes untouched");
    test_check(w.len == count_pos + 5,
               "9p codec: patch_u32 does not advance the write cursor");

    /* A patch that runs past what has actually been written must be refused
       rather than scribble into uninitialized buffer space. */
    Axl9pWriter w2;
    axl_9p_w_init(&w2, buf, sizeof(buf));
    axl_9p_w_u8(&w2, 1);
    axl_9p_w_u8(&w2, 2);
    axl_9p_w_u8(&w2, 3);
    axl_9p_w_patch_u32(&w2, 0, 0xFFFFFFFFu);
    test_check(w2.overflow, "9p codec: patch_u32 past the write cursor sets overflow");
    test_check(buf[0] == 1 && buf[1] == 2 && buf[2] == 3,
               "9p codec: refused patch_u32 wrote nothing");
}

/* axl_9p_w_reserve -- claim payload space in place so a producer that isn't
   an axl_9p_w_* call (a file read) can write STRAIGHT into the reply buffer.
   Without it, Rread would need a second msize-sized staging buffer and a
   second copy of every byte served. */
static void
test_9p_w_reserve(void)
{
    uint8_t buf[16];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    axl_9p_w_u8(&w, 0x5A);

    uint8_t *p = axl_9p_w_reserve(&w, 4);
    test_check(p == buf + 1, "9p codec: reserve returns the cursor position");
    test_check(w.len == 5, "9p codec: reserve advances the cursor past the run");
    test_check(!w.overflow, "9p codec: a reserve that fits does not set overflow");
    p[0] = 0xDE; p[1] = 0xAD; p[2] = 0xBE; p[3] = 0xEF;
    axl_9p_w_u8(&w, 0xA5);
    test_check(buf[0] == 0x5A && buf[1] == 0xDE && buf[4] == 0xEF && buf[5] == 0xA5,
               "9p codec: writes resume immediately after a reserved run");

    /* Over-capacity reserve: NULL, overflow set, cursor untouched so the
       caller can still finish or re-begin the message deterministically. */
    uint8_t *q = axl_9p_w_reserve(&w, sizeof(buf));
    test_check(q == NULL, "9p codec: an over-capacity reserve returns NULL");
    test_check(w.overflow, "9p codec: an over-capacity reserve sets overflow");
    test_check(w.len == 6, "9p codec: a refused reserve does not advance the cursor");

    /* Exactly-fits is not an off-by-one refusal. */
    Axl9pWriter w2;
    axl_9p_w_init(&w2, buf, sizeof(buf));
    test_check(axl_9p_w_reserve(&w2, sizeof(buf)) == buf,
               "9p codec: a reserve of exactly the capacity succeeds");
    test_check(w2.len == sizeof(buf) && !w2.overflow,
               "9p codec: an exact-capacity reserve consumes the whole buffer");
}

/* s9p_read_clamp -- Tread's (offset, count) are both wire-controlled. The
   offset is 64-bit and may name a byte no size_t index could hold; the count
   may exceed both the file and the reply. This is the one place that decides
   how many bytes get copied, so it is pinned exactly. */
static void
test_9p_read_clamp(void)
{
    size_t off = 0xDEAD;

    test_check(s9p_read_clamp(0, 100, 10, 1000, &off) == 10,
               "9p read clamp: a count past EOF is clamped to the bytes available");
    test_check(off == 0, "9p read clamp: in-range offset is passed through");

    test_check(s9p_read_clamp(4, 2, 10, 1000, &off) == 2,
               "9p read clamp: a count inside the file is granted as-is");
    test_check(off == 4, "9p read clamp: mid-file offset is passed through");

    test_check(s9p_read_clamp(4, 100, 10, 3, &off) == 3,
               "9p read clamp: the reply's remaining room bounds the count");

    off = 0xDEAD;
    test_check(s9p_read_clamp(10, 100, 10, 1000, &off) == 0,
               "9p read clamp: an offset exactly at EOF reads zero bytes");
    test_check(off == 0, "9p read clamp: a zero-byte result reports offset 0");

    off = 0xDEAD;
    test_check(s9p_read_clamp(0xFFFFFFFFFFFFFFFFULL, 100, 10, 1000, &off) == 0,
               "9p read clamp: a 64-bit offset far past EOF reads zero bytes");
    test_check(off == 0,
               "9p read clamp: an un-narrowable offset never reaches the reader");

    /* The three zero-byte results below all reach the clamp with an
       IN-RANGE offset, which is the only way the "*out_offset is 0 whenever
       the return is 0" half of the contract can be observed -- and the only
       way a caller that reads the offset without checking the count can be
       caught. Each pre-seeds `off` so a stale write is visible. */
    off = 0xDEAD;
    test_check(s9p_read_clamp(0, 0, 10, 1000, &off) == 0,
               "9p read clamp: a zero count reads zero bytes");
    test_check(off == 0,
               "9p read clamp: a zero count reports offset 0, not the in-range offset");

    off = 0xDEAD;
    test_check(s9p_read_clamp(0, 100, 0, 1000, &off) == 0,
               "9p read clamp: an empty file reads zero bytes at offset 0");
    test_check(off == 0, "9p read clamp: an empty file reports offset 0");

    off = 0xDEAD;
    test_check(s9p_read_clamp(5, 100, 10, 0, &off) == 0,
               "9p read clamp: no room in the reply reads zero bytes");
    test_check(off == 0,
               "9p read clamp: no room reports offset 0, not the in-range offset");
}

/* s9p_readdir_resume -- the Treaddir cursor is wire-controlled and a client
   may hand back an offset no reply ever emitted (past the end, mid-record,
   or moving backwards). The mapping must be total: every uint64 lands on a
   position rather than faulting. Record k carries dirent offset k+1, so a
   cursor of N means N records already delivered: 0 = ".", 1 = "..", the
   rest real entries. */
static void
test_9p_readdir_resume(void)
{
    bool     dot = false, dotdot = false;
    uint64_t skip = 0xDEAD;

    s9p_readdir_resume(0, &dot, &dotdot, &skip);
    test_check(dot && dotdot && skip == 0,
               "9p readdir resume: cursor 0 owes both synthetic records");

    s9p_readdir_resume(1, &dot, &dotdot, &skip);
    test_check(!dot && dotdot && skip == 0,
               "9p readdir resume: cursor 1 owes only \"..\"");

    s9p_readdir_resume(2, &dot, &dotdot, &skip);
    test_check(!dot && !dotdot && skip == 0,
               "9p readdir resume: cursor 2 starts at the first real entry");

    s9p_readdir_resume(3, &dot, &dotdot, &skip);
    test_check(!dot && !dotdot && skip == 1,
               "9p readdir resume: cursor 3 skips one real entry");

    s9p_readdir_resume(1000, &dot, &dotdot, &skip);
    test_check(!dot && !dotdot && skip == 998,
               "9p readdir resume: a far cursor skips cursor-2 real entries");

    s9p_readdir_resume(0xFFFFFFFFFFFFFFFFULL, &dot, &dotdot, &skip);
    test_check(!dot && !dotdot && skip == 0xFFFFFFFFFFFFFFFDULL,
               "9p readdir resume: a made-up 64-bit cursor maps without wrapping");
}

/* s9p_dirent_put -- one Rreaddir record. The fill loop stops when the next
   record doesn't fit, and it MUST stop at a record boundary: a truncated
   record would desync the client's parse of the whole reply, so a refusal
   has to write nothing at all. */
static void
test_9p_dirent_put(void)
{
    uint8_t qid[AXL_9P_QID_LEN];
    for (size_t i = 0; i < AXL_9P_QID_LEN; i++) {
        qid[i] = (uint8_t)(0xA0 + i);
    }

    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    test_check(s9p_dirent_put(&w, sizeof(buf), qid, 3, 8, "ab"),
               "9p dirent: a record that fits is written");
    test_check(w.len == AXL_9P_QID_LEN + 8 + 1 + 2 + 2,
               "9p dirent: record length is qid13 + off8 + type1 + strlen-prefixed name");
    test_check(buf[0] == 0xA0 && buf[12] == 0xAC,
               "9p dirent: the qid is copied verbatim, first field");
    test_check(buf[13] == 3 && buf[14] == 0 && buf[20] == 0,
               "9p dirent: the dirent offset is little-endian u64");
    test_check(buf[21] == 8, "9p dirent: the DT_* type byte follows the offset");
    test_check(buf[22] == 2 && buf[23] == 0,
               "9p dirent: the name carries a little-endian u16 length prefix");
    test_check(buf[24] == 'a' && buf[25] == 'b',
               "9p dirent: the name bytes follow the length prefix");

    /* One byte short of the record's 26 bytes: refused, nothing written. */
    Axl9pWriter w2;
    axl_9p_w_init(&w2, buf, sizeof(buf));
    test_check(!s9p_dirent_put(&w2, 25, qid, 3, 8, "ab"),
               "9p dirent: a record one byte over the room budget is refused");
    test_check(w2.len == 0, "9p dirent: a refused record writes nothing");
    test_check(!w2.overflow,
               "9p dirent: a refused record leaves the writer usable (not overflowed)");

    /* Exactly the record's size is not an off-by-one refusal. */
    Axl9pWriter w3;
    axl_9p_w_init(&w3, buf, sizeof(buf));
    test_check(s9p_dirent_put(&w3, 26, qid, 3, 8, "ab"),
               "9p dirent: a record exactly filling the room budget is written");

    /* Room to spare but no capacity in the writer -- same clean refusal. */
    uint8_t small[20];
    Axl9pWriter w4;
    axl_9p_w_init(&w4, small, sizeof(small));
    test_check(!s9p_dirent_put(&w4, 1000, qid, 3, 8, "ab"),
               "9p dirent: a record past the writer's capacity is refused");
    test_check(w4.len == 0 && !w4.overflow,
               "9p dirent: a capacity refusal also writes nothing and does not overflow");

    /* The "." record a directory read always leads with, byte-exact. */
    Axl9pWriter w5;
    axl_9p_w_init(&w5, buf, sizeof(buf));
    test_check(s9p_dirent_put(&w5, sizeof(buf), qid, 1, 4, "."),
               "9p dirent: the synthetic \".\" record is written");
    test_check(w5.len == 25 && buf[13] == 1 && buf[21] == 4
               && buf[22] == 1 && buf[23] == 0 && buf[24] == '.',
               "9p dirent: \".\" is offset 1, DT_DIR, one name byte");
}

/* s9p_path_up -- the ".." step, and the one place a walk is stopped from
   escaping the export root. Its declared contract is that a false return
   leaves the caller's buffer UNTOUCHED, which is what lets a caller keep
   using the buffer as "the path I was already at" (Twalk breaks out of its
   loop still holding `cur`; Treaddir's ".." falls back to the directory
   itself at the top of the tree). That is the half worth pinning: the
   success case is exercised constantly by the 9p integration suite, while a
   silent truncation on the failure path would only ever show up as a
   mangled path much later. */
static void
test_9p_path_up(void)
{
    char path[64];

    axl_strlcpy(path, "fs0:\\a\\b", sizeof(path));
    test_check(s9p_path_up("fs0:", path, sizeof(path))
               && axl_strcmp(path, "fs0:\\a") == 0,
               "9p path up: a step inside the root yields the parent");

    axl_strlcpy(path, "fs0:", sizeof(path));
    test_check(!s9p_path_up("fs0:", path, sizeof(path)),
               "9p path up: a step at the export root is refused");
    test_check(axl_strcmp(path, "fs0:") == 0,
               "9p path up: a refusal at the root leaves the buffer untouched");

    /* Walk a nested path all the way up: each step succeeds until the root,
       and the step that would escape refuses with the buffer still holding
       the root. This is exactly the sequence Treaddir's ".." record and
       Twalk's ".." component depend on. */
    axl_strlcpy(path, "fs0:\\a\\b\\c", sizeof(path));
    test_check(s9p_path_up("fs0:", path, sizeof(path))
               && s9p_path_up("fs0:", path, sizeof(path))
               && s9p_path_up("fs0:", path, sizeof(path))
               && axl_strcmp(path, "fs0:") == 0,
               "9p path up: three steps from fs0:\\a\\b\\c land exactly on the root");
    test_check(!s9p_path_up("fs0:", path, sizeof(path))
               && axl_strcmp(path, "fs0:") == 0,
               "9p path up: the step that would escape refuses, buffer intact");
}

/* s9p_comp_is_safe -- the ONE guard standing between a wire-supplied path
   component and axl_path_join. Twalk has always gone through it; Tlcreate,
   Tmkdir and Trename now do too, which makes a hole here a hole in five
   handlers rather than one. The nasty case is the backslash: AxlFs treats
   BOTH '/' and '\\' as separators (is_sep in axl-path.c), so a slash-only
   check would let "..\\..\\secret" smuggle a multi-level escape past it. */
static void
test_9p_comp_is_safe(void)
{
    test_check(s9p_comp_is_safe("hello.txt", 9, 256),
               "9p comp: an ordinary component is accepted");
    test_check(s9p_comp_is_safe("..", 2, 256),
               "9p comp: \"..\" is accepted here -- the root guard is s9p_path_up's job");

    test_check(!s9p_comp_is_safe("", 0, 256),
               "9p comp: an empty component is rejected");
    test_check(!s9p_comp_is_safe("a/b", 3, 256),
               "9p comp: a component containing '/' is rejected");
    test_check(!s9p_comp_is_safe("a\\b", 3, 256),
               "9p comp: a component containing '\\' is rejected");
    test_check(!s9p_comp_is_safe("..\\..\\secret", 12, 256),
               "9p comp: a backslash multi-level escape is rejected");
    test_check(!s9p_comp_is_safe("../../secret", 12, 256),
               "9p comp: a slash multi-level escape is rejected");

    /* Truncation: axl_9p_r_str returns the ACTUAL on-wire length, so a
       wire_n at or past `cap` means the copy in `comp` is a PREFIX of what
       the client sent -- accepting it would operate on a name nobody asked
       for. Pinned at the exact boundary in both directions. */
    test_check(!s9p_comp_is_safe("abc", 8, 4),
               "9p comp: a component truncated against cap is rejected");
    test_check(!s9p_comp_is_safe("abc", 4, 4),
               "9p comp: wire_n exactly at cap is rejected (no room for the NUL)");
    test_check(s9p_comp_is_safe("abc", 3, 4),
               "9p comp: wire_n one below cap is accepted");

    /* An embedded NUL copies cleanly but desyncs strlen(comp) from wire_n,
       which is how a "safe" prefix could carry a different real name. */
    char embedded[8] = { 'a', 'b', '\0', 'c', '\0', 0, 0, 0 };
    test_check(!s9p_comp_is_safe(embedded, 4, sizeof(embedded)),
               "9p comp: a component with an embedded NUL is rejected");
}

/* s9p_type_is_mutating -- the read-only gate's whole coverage claim. A type
   missing from it is a write that lands on a --ro export; a type wrongly ON
   it is a read that stops working. Both directions are pinned, message type
   by message type, because the gate itself cannot be reached without a
   socket until the Task-6 harness exists. */
static void
test_9p_type_is_mutating(void)
{
    test_check(s9p_type_is_mutating(AXL_9P_TLCREATE),
               "9p ro gate: Tlcreate is gated");
    test_check(s9p_type_is_mutating(AXL_9P_TWRITE),
               "9p ro gate: Twrite is gated");
    test_check(s9p_type_is_mutating(AXL_9P_TMKDIR),
               "9p ro gate: Tmkdir is gated");
    test_check(s9p_type_is_mutating(AXL_9P_TREMOVE),
               "9p ro gate: Tremove is gated");
    test_check(s9p_type_is_mutating(AXL_9P_TRENAME),
               "9p ro gate: Trename is gated");
    test_check(s9p_type_is_mutating(AXL_9P_TSETATTR),
               "9p ro gate: Tsetattr is gated");

    test_check(!s9p_type_is_mutating(AXL_9P_TVERSION),
               "9p ro gate: Tversion is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TATTACH),
               "9p ro gate: Tattach is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TWALK),
               "9p ro gate: Twalk is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TCLUNK),
               "9p ro gate: Tclunk is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TGETATTR),
               "9p ro gate: Tgetattr is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TREAD),
               "9p ro gate: Tread is not gated");
    test_check(!s9p_type_is_mutating(AXL_9P_TREADDIR),
               "9p ro gate: Treaddir is not gated");
    /* Tlopen makes its OWN, finer check: a read-only open of a file on a
       read-only export must still succeed, so blanket-gating it here would
       break every --ro mount. */
    test_check(!s9p_type_is_mutating(AXL_9P_TLOPEN),
               "9p ro gate: Tlopen is not blanket-gated (it checks its access mode)");
    /* Tfsync writes nothing of its own; POSIX permits fsync on a read-only
       descriptor. */
    test_check(!s9p_type_is_mutating(AXL_9P_TFSYNC),
               "9p ro gate: Tfsync is not gated");
    test_check(!s9p_type_is_mutating(99),
               "9p ro gate: an unknown type is not gated (it is answered EPROTO)");
}

/* s9p_write_bounds -- the single check between a client-supplied `count` and
   a read past the receive buffer. The framing loop proves the FRAME is
   complete; nothing proves the count field inside it is honest, so a count
   of 0xFFFFFFFF in a 30-byte frame must be refused before `data` is ever
   formed. The offset half is the same 64-bit narrowing hazard s9p_read_clamp
   answers, except a write also has to survive offset + count. */
static void
test_9p_write_bounds(void)
{
    size_t off = 0xDEAD;

    test_check(s9p_write_bounds(0, 4, 4, &off) && off == 0,
               "9p write bounds: a count exactly filling the frame is accepted");
    test_check(s9p_write_bounds(10, 4, 8, &off) && off == 10,
               "9p write bounds: a count inside the frame passes the offset through");

    off = 0xDEAD;
    test_check(!s9p_write_bounds(0, 5, 4, &off),
               "9p write bounds: a count one byte past the frame is refused");
    test_check(off == 0, "9p write bounds: a refusal zeroes the narrowed offset");

    off = 0xDEAD;
    test_check(!s9p_write_bounds(0, 0xFFFFFFFFu, 23, &off),
               "9p write bounds: a 4 GiB count in a 30-byte frame is refused");
    test_check(off == 0,
               "9p write bounds: the hostile-count refusal zeroes the offset too");

    /* A zero-length write is legal 9P -- answered Rwrite(0), not an error --
       and stays legal at any offset, because nothing is dereferenced. */
    off = 0xDEAD;
    test_check(s9p_write_bounds(0, 0, 0, &off) && off == 0,
               "9p write bounds: an empty write against an empty payload is accepted");
    test_check(s9p_write_bounds(1000, 0, 0, &off) && off == 1000,
               "9p write bounds: an empty write at a far offset is accepted");

    /* offset + count must not wrap a size_t: the pair is handed to a
       size_t-indexed positional write. */
    off = 0xDEAD;
    test_check(!s9p_write_bounds(0xFFFFFFFFFFFFFFFFULL, 1, 4, &off),
               "9p write bounds: a 64-bit offset that cannot address the payload is refused");
    test_check(off == 0,
               "9p write bounds: the overflow refusal zeroes the offset");
    test_check(!s9p_write_bounds((uint64_t)SIZE_MAX, 1, 4, &off),
               "9p write bounds: offset SIZE_MAX with one payload byte overflows and is refused");
    test_check(s9p_write_bounds((uint64_t)SIZE_MAX - 1u, 1, 4, &off)
               && off == SIZE_MAX - 1u,
               "9p write bounds: the largest non-overflowing offset is accepted");
}

/* s9p_write_grow_ok -- the OTHER half of the same hazard s9p_setattr_size_ok
   guards, and the cheaper half to exploit. A positional write may legally
   land past EOF; the UEFI write then grows the file and the FAT driver
   physically zero-fills the gap, so a 24-byte Twrite naming a 4 GiB offset
   stalls the whole single-threaded loop for the entire fill and then leaves
   the volume full. Same ceiling, same errno (EFBIG) as Tsetattr. */
static void
test_9p_write_grow_ok(void)
{
    /* Writes that land entirely inside the file grow nothing. */
    test_check(s9p_write_grow_ok(0, 4, 100),
               "9p write grow: a write inside the file is allowed");
    test_check(s9p_write_grow_ok(96, 4, 100),
               "9p write grow: a write ending exactly at EOF is allowed");
    test_check(s9p_write_grow_ok(1000, 10, 1024u * 1024u),
               "9p write grow: a write deep inside a large file is allowed");
    test_check(s9p_write_grow_ok(100, 0, 100),
               "9p write grow: an empty write at EOF is allowed");

    /* Ordinary appends, right up to the ceiling. */
    test_check(s9p_write_grow_ok(0, 4, 0),
               "9p write grow: a small append to an empty file is allowed");
    test_check(s9p_write_grow_ok(AXL_9P_SERVER_MAX_GROW - 1u, 1, 0),
               "9p write grow: a write ending exactly at the ceiling is allowed");
    test_check(!s9p_write_grow_ok(AXL_9P_SERVER_MAX_GROW, 1, 0),
               "9p write grow: a write ending one byte over the ceiling is refused");
    test_check(s9p_write_grow_ok(100u + AXL_9P_SERVER_MAX_GROW - 1u, 1, 100),
               "9p write grow: the ceiling is measured from the CURRENT length");
    test_check(!s9p_write_grow_ok(100u + AXL_9P_SERVER_MAX_GROW, 1, 100),
               "9p write grow: one byte over the ceiling from a non-zero length is refused");

    /* The attack the guard exists for: a one-byte payload at a far offset. */
    test_check(!s9p_write_grow_ok(0xFFFFFFFEull, 1, 0),
               "9p write grow: a one-byte write at a 4 GiB offset is refused");
    test_check(!s9p_write_grow_ok(0x7FFFFFFFFFFFFFFFull, 1, 0),
               "9p write grow: a one-byte write at a 8 EiB offset is refused");
    test_check(!s9p_write_grow_ok(0xFFFFFFFFFFFFFFFFull, 1, 0),
               "9p write grow: an offset that overflows when the count is added is refused");
}

/* s9p_write_grow_needs_size -- whether the bound above actually has to know
   the file's length. When it does not, Twrite skips a firmware size query
   entirely, so this predicate is what makes an ordinary sequential append
   cost nothing extra. It cannot be tested by counting calls (the query is
   not observable from here), so it is tested as what it is: a claim about
   s9p_write_grow_ok's answer being INDEPENDENT of cur_size, checked by
   asking that function for both extremes and requiring them to agree. */
static void
test_9p_write_grow_needs_size(void)
{
    test_check(!s9p_write_grow_needs_size(0, 4),
               "9p write grow: a small write needs no size query");
    test_check(!s9p_write_grow_needs_size(AXL_9P_SERVER_MAX_GROW - 1u, 1),
               "9p write grow: a write ending exactly at the ceiling needs no size query");
    test_check(s9p_write_grow_needs_size(AXL_9P_SERVER_MAX_GROW, 1),
               "9p write grow: a write ending past the ceiling needs the size query");
    test_check(s9p_write_grow_needs_size(0xFFFFFFFEull, 1),
               "9p write grow: a far-offset write needs the size query");
    test_check(!s9p_write_grow_needs_size(0xFFFFFFFFFFFFFFFFull, 1),
               "9p write grow: an overflowing write is refused without a size query");

    /* The property the skip rests on: wherever the predicate says no query
       is needed, the bound must answer the same for cur_size 0 (what the
       handler passes when it skips) as for any other length. Checked at
       both extremes, so a regression that made the answer size-dependent
       below the threshold shows up here rather than as a wrong EFBIG in
       production. */
    test_check(s9p_write_grow_ok(0, 4, 0) == s9p_write_grow_ok(0, 4, 1000000)
               && s9p_write_grow_ok(0, 4, 0),
               "9p write grow: a small write is allowed regardless of the file's length");
    test_check(s9p_write_grow_ok(AXL_9P_SERVER_MAX_GROW - 1u, 1, 0)
                   == s9p_write_grow_ok(AXL_9P_SERVER_MAX_GROW - 1u, 1,
                                        0xFFFFFFFFFFFFFFFFull)
               && s9p_write_grow_ok(AXL_9P_SERVER_MAX_GROW - 1u, 1, 0),
               "9p write grow: a write at the ceiling is allowed regardless of the file's length");
    test_check(!s9p_write_grow_ok(0xFFFFFFFFFFFFFFFFull, 1, 0)
               && !s9p_write_grow_ok(0xFFFFFFFFFFFFFFFFull, 1,
                                     0xFFFFFFFFFFFFFFFFull),
               "9p write grow: an overflowing write is refused regardless of the file's length");
}

/* s9p_setattr_size_ok -- Tsetattr's size comes straight off the wire, and
   axl_file_truncate documents a grow as O(size) PHYSICAL zero-fill on the
   FAT driver every UEFI box ships. This server is one loop, so an unbounded
   grow is eleven wire bytes that stall every connection for minutes. Shrinks
   move no data and stay unbounded. */
static void
test_9p_setattr_size_ok(void)
{
    test_check(s9p_setattr_size_ok(0, 1024u * 1024u * 1024u),
               "9p setattr bound: truncating a 1 GiB file to zero is allowed");
    test_check(s9p_setattr_size_ok(100, 100),
               "9p setattr bound: a no-op resize is allowed");
    test_check(s9p_setattr_size_ok(99, 100),
               "9p setattr bound: a one-byte shrink is allowed");

    test_check(s9p_setattr_size_ok(AXL_9P_SERVER_MAX_GROW, 0),
               "9p setattr bound: a grow of exactly the ceiling is allowed");
    test_check(!s9p_setattr_size_ok((uint64_t)AXL_9P_SERVER_MAX_GROW + 1u, 0),
               "9p setattr bound: one byte over the ceiling is refused");
    test_check(s9p_setattr_size_ok(100u + AXL_9P_SERVER_MAX_GROW, 100),
               "9p setattr bound: the ceiling is measured from the CURRENT length");
    test_check(!s9p_setattr_size_ok(101u + AXL_9P_SERVER_MAX_GROW, 100),
               "9p setattr bound: one byte over the ceiling from a non-zero length is refused");

    test_check(!s9p_setattr_size_ok(0xFFFFFFFFFFFFFFFFULL, 0),
               "9p setattr bound: a 16 EiB grow is refused");
    test_check(!s9p_setattr_size_ok(4ull * 1024 * 1024 * 1024, 0),
               "9p setattr bound: a 4 GiB grow is refused");
}

/* A 9P fid holds its AxlFileView open for the fid's whole lifetime, and a
   view caches the file length at ITS open and serves from cached pages. So
   a fid that is already open when something else writes the file would
   report a stale size over stale bytes.

   The server used to patch that itself, marking its own fids' views after
   each mutation -- which covered writers INSIDE one Axl9pServer and no
   others. A second Axl9pServer over the same root (which the selftest
   really does publish, rw + ro) was invisible to it, as was every non-9P
   writer in the image. That best effort now belongs to AxlFileView (see
   src/fs/axl-file-gen.h) and this pins that a 9P fid inherits it, using a
   writer the server is given no chance to notice: a plain
   axl_file_set_contents that goes nowhere near a handler.

   What the EXPORT promises a client is close-to-open, not coherence (see
   src/9p/README.md) -- these assertions cover the in-image best effort,
   which is strictly more than the contract requires.

   Needs a writable volume but no socket, which is the only reason it can
   run before the Task-6 harness. */
static void
test_9p_view_coherence(void)
{
    static const char kFile[]  = "fs0:\\axl_9p_stale.tmp";
    static const char kOther[] = "fs0:\\axl_9p_other.tmp";
    static const char kUpper[] = "FS0:\\AXL_9P_STALE.TMP";
    static const char kShort[] = "abc";
    static const char kLong[]  = "abcdef";
    static const char kCase[]  = "xyzxyz";

    AxlLoop     *loop = axl_loop_new();
    Axl9pServer *srv  = NULL;
    test_check(axl_9p_server_new(loop, "fs0:\\", false, &srv) == AXL_OK
               && srv != NULL,
               "9p view coherence: server created with its shared page cache");

    S9pFid *f = NULL;
    if (srv != NULL
        && axl_file_set_contents(kFile, kShort, sizeof(kShort) - 1) == AXL_OK) {
        S9pConn *conn = &srv->conns[0];
        conn->active = true;
        f = s9p_fid_alloc(conn, 1, kFile, false);
        if (f != NULL) {
            f->view = axl_file_view_open_cached(kFile, srv->page_cache);
            f->open = (f->view != NULL);
        }
    }

    if (f != NULL && f->view != NULL) {
        char body[16];

        test_check(axl_file_view_size(f->view) == sizeof(kShort) - 1,
                   "9p view coherence: a fresh view reports the file's length at open");

        /* Read once BEFORE the rewrite so the file's first page really is
           resident in the shared cache. Without this the only staleness on
           offer would be the cached LENGTH, and an implementation that
           merely re-statted would look correct. */
        axl_memset(body, 0, sizeof(body));
        test_check(axl_file_view_read(f->view, 0, body, sizeof(kShort) - 1)
                       == sizeof(kShort) - 1
                   && axl_strcmp(body, kShort) == 0,
                   "9p view coherence: the view reads the original bytes (page now resident)");

        /* A writer the server knows nothing about -- the shape a second
           Axl9pServer over the same root takes from this one's point of
           view. */
        test_check(axl_file_set_contents(kFile, kLong, sizeof(kLong) - 1) == AXL_OK,
                   "9p view coherence: the file is rewritten longer behind the view");
        test_check(axl_file_view_size(f->view) == sizeof(kLong) - 1,
                   "9p view coherence: the fid's view reports the NEW length, unprompted");
        axl_memset(body, 0, sizeof(body));
        test_check(axl_file_view_read(f->view, 0, body, sizeof(kLong) - 1)
                       == sizeof(kLong) - 1,
                   "9p view coherence: the view reads the full new length");
        test_check(axl_strcmp(body, kLong) == 0,
                   "9p view coherence: the view returns the NEW bytes, not the resident page");

        /* FAT is case-insensitive, so a write spelled "FS0:\AXL_9P_STALE.TMP"
           hits the SAME file a fid walked as "fs0:\axl_9p_stale.tmp" reads.
           An exact-compare key would leave this view stale forever. */
        test_check(axl_file_set_contents(kUpper, kCase, sizeof(kCase) - 1) == AXL_OK,
                   "9p view coherence: rewrote the file under a differently-cased path");
        axl_memset(body, 0, sizeof(body));
        test_check(axl_file_view_read(f->view, 0, body, sizeof(kCase) - 1)
                       == sizeof(kCase) - 1
                   && axl_strcmp(body, kCase) == 0,
                   "9p view coherence: a differently-cased write still reaches the view");

        /* A mutation somewhere ELSE must not disturb this fid -- the
           negative the removed per-server marking used to carry. A
           degenerate key would satisfy every assertion above while
           re-opening every view in the image on every write. */
        AxlFileViewStats st_before;
        AxlFileViewStats st_after;
        axl_file_view_stats(f->view, &st_before);
        test_check(axl_file_set_contents(kOther, kShort, sizeof(kShort) - 1) == AXL_OK,
                   "9p view coherence: wrote an unrelated path");
        axl_memset(body, 0, sizeof(body));
        size_t again = axl_file_view_read(f->view, 0, body, sizeof(kCase) - 1);
        axl_file_view_stats(f->view, &st_after);
        test_check(again == sizeof(kCase) - 1 && axl_strcmp(body, kCase) == 0
                   && st_after.preads == st_before.preads,
                   "9p view coherence: an unrelated write leaves this fid's view untouched");
        test_check(axl_file_delete(kOther) == AXL_OK,
                   "9p view coherence: cleaned up the unrelated path");

        /* Removed under the fid: the view has to say so rather than let an
           empty read pass for EOF, and has to keep saying the same thing. */
        test_check(axl_file_delete(kFile) == AXL_OK,
                   "9p view coherence: deleted the file under the open fid");
        test_check(axl_file_view_refresh(f->view) != AXL_OK,
                   "9p view coherence: refresh reports the file is gone");
        test_check(axl_file_view_refresh(f->view) != AXL_OK,
                   "9p view coherence: the next refresh reports the SAME thing (no EIO-then-EBADF)");
    } else {
        /* No writable fs0: here -- one balancer per check above. */
        test_skip_n(14, "9p view coherence: fs0: not writable");
    }

    axl_9p_server_free(srv);
    axl_loop_free(loop);
}

/* s9p_dir_is_empty -- what lets Tremove answer a non-empty rmdir with
   ENOTEMPTY instead of a blanket EIO. The trap it has to survive is that a
   UEFI FAT directory carries on-disk "." and ".." entries, so a naive
   "axl_dir_read yielded something" test would call every directory
   non-empty and make rmdir impossible. */
static void
test_9p_dir_is_empty(void)
{
    static const char kDir[]  = "fs0:\\axl9pemp.tmp";
    static const char kInner[] = "fs0:\\axl9pemp.tmp\\f.txt";

    test_check(!s9p_dir_is_empty("fs0:\\axl_9p_no_such_dir.tmp"),
               "9p dir empty: a directory that does not exist is not empty");

    if (axl_dir_mkdir(kDir) == AXL_OK) {
        test_check(s9p_dir_is_empty(kDir),
                   "9p dir empty: a freshly created directory is empty despite on-disk . and ..");
        test_check(axl_file_set_contents(kInner, "x", 1) == AXL_OK,
                   "9p dir empty: a file is created inside it");
        test_check(!s9p_dir_is_empty(kDir),
                   "9p dir empty: a directory holding one file is not empty");
        axl_file_delete(kInner);
        test_check(s9p_dir_is_empty(kDir),
                   "9p dir empty: it is empty again once the file is deleted");
        axl_dir_rmdir(kDir);
    } else {
        /* No writable fs0: here -- one balancer per check above. */
        test_skip_n(4, "9p dir empty: fs0: not writable");
    }
}

/* s9p_fid_release -- Tlopen gives a fid something to OWN (an AxlFileView for
   a file, an AxlDir for a directory), and three sites drop fids: Tclunk, the
   Tversion session reset, and connection reap. Each used to open-code a
   memset, which was correct only while a fid held nothing but a path string
   and became a leak of a live handle the moment it didn't -- the reset is
   the nastiest, since a client may renegotiate the msize in the middle of a
   directory walk. Routing all three through one release path is the fix;
   this is where it is actually proved, by building the open state by hand on
   a real server's connection slot and checking the allocator gets every
   block back. It needs no socket, which is the only reason it can run before
   the Task-6 harness exists. */
static void
test_9p_fid_release_closes_open_state(void)
{
    static const char kFile[] = "fs0:\\axl_9p_fid_rel.tmp";
    static const char kBody[] = "9p fid release fixture";

    AxlLoop     *loop = axl_loop_new();
    Axl9pServer *srv  = NULL;
    test_check(axl_9p_server_new(loop, "fs0:\\", false, &srv) == AXL_OK
               && srv != NULL,
               "9p fid release: server created with its shared page cache");

    if (srv != NULL
        && axl_file_set_contents(kFile, kBody, sizeof(kBody) - 1) == AXL_OK) {
        S9pConn    *conn = &srv->conns[0];
        AxlMemStats before, after;
        axl_mem_get_stats(&before);

        /* O_RDWR shape: the SAME fid owns a reader and a writer, and both
           have to come back through the one release path. */
        S9pFid *ff = s9p_fid_alloc(conn, 1, kFile, false);
        if (ff != NULL) {
            ff->view    = axl_file_view_open_cached(kFile, srv->page_cache);
            ff->wstream = axl_fopen(kFile, "w");
            ff->open    = (ff->view != NULL && ff->wstream != NULL);
        }
        test_check(ff != NULL && ff->view != NULL,
                   "9p fid release: a file fid holds an open page-cached view");
        test_check(ff != NULL && ff->wstream != NULL,
                   "9p fid release: the same fid holds an open write stream");

        S9pFid *df = s9p_fid_alloc(conn, 2, "fs0:\\", true);
        if (df != NULL) {
            df->dir  = axl_dir_open("fs0:\\");
            df->open = (df->dir != NULL);
        }
        test_check(df != NULL && df->dir != NULL,
                   "9p fid release: a directory fid holds an open iterator");

        s9p_fid_reset_all(conn);
        axl_mem_get_stats(&after);
        test_check(after.count == before.count,
                   "9p fid release: reset_all closes both handle kinds, no leak");
        test_check(s9p_fid_get(conn, 1) == NULL && s9p_fid_get(conn, 2) == NULL,
                   "9p fid release: both fid slots are free again");

        /* Tclunk's path -- the ordinary per-fid close. Write-ONLY here, the
           shape a Twrite fid actually has, so a close_open that only ever
           released `view` would show up as a leak rather than pass by
           accident on a fid that also had one. */
        axl_mem_get_stats(&before);
        S9pFid *cf = s9p_fid_alloc(conn, 3, kFile, false);
        if (cf != NULL) {
            cf->wstream = axl_fopen(kFile, "w");
            cf->open    = (cf->wstream != NULL);
        }
        bool clunked = s9p_fid_clunk(conn, 3);
        axl_mem_get_stats(&after);
        test_check(clunked && after.count == before.count,
                   "9p fid release: clunk closes the fid's view cleanly, no leak");
        test_check(s9p_fid_get(conn, 3) == NULL,
                   "9p fid release: the clunked fid slot is free again");

        /* Connection reap -- the one site whose correctness is an ORDERING
           property: the fid table has to be released BEFORE s9p_reap's
           blanket memset, which would otherwise drop the last reference to
           every open handle. Driven on a second slot with no socket and no
           buffers, all three of which s9p_reap NULL-checks. */
        S9pConn *rc = &srv->conns[1];
        rc->active = true;
        axl_mem_get_stats(&before);
        S9pFid *rf = s9p_fid_alloc(rc, 4, kFile, false);
        if (rf != NULL) {
            rf->view    = axl_file_view_open_cached(kFile, srv->page_cache);
            rf->wstream = axl_fopen(kFile, "w");
            rf->open    = (rf->view != NULL && rf->wstream != NULL);
        }
        s9p_reap(rc);
        axl_mem_get_stats(&after);
        test_check(after.count == before.count,
                   "9p fid release: reap releases fids before zeroing the conn, no leak");
        test_check(!rc->active && rc->srv == srv,
                   "9p fid release: reap leaves the slot inactive with its server back-pointer");

        axl_file_delete(kFile);
    } else {
        /* No writable fs0: here -- one balancer per conditional above. */
        test_skip_n(9, "9p fid release: fs0: not writable");
    }

    axl_9p_server_free(srv);
    axl_loop_free(loop);
}

/* Regression: a fid that was written to and then clunked must not be
   answered as if the bytes landed. s9p_fid_close_open called
   axl_fclose(f->wstream) and threw the status away -- and axl_fclose is not
   a durability point at all (it drains the AXL-side buffer through
   stream_drain and never invokes the stream's flush), so a client that
   wrote and clunked without an explicit Tfsync got Rclunk for data a full
   volume or write-protected media had dropped. Tclunk is the LAST chance
   the protocol offers to say so.

   Driven at the fid layer, where the decision is made: the wire mapping
   needs a live socket (s9p_send_reply borrows conn->txbuf until the send
   completes and reaps the connection when there is none), which is the
   integration harness's job -- but that harness would need a flush-failing
   export in the guest to reach this at all. */
static void
test_9p_clunk_reports_a_failed_flush(void)
{
    if (!ff_fs_up()) {
        test_skip_n(4, "9p clunk flush-fail (no shell map for the published volume)");
        return;
    }

    AxlLoop     *loop = axl_loop_new();
    Axl9pServer *srv  = NULL;
    if (axl_9p_server_new(loop, FF_MAP ":\\", false, &srv) == AXL_OK
        && srv != NULL) {
        S9pConn *conn = &srv->conns[0];

        S9pFid *wf = s9p_fid_alloc(conn, 1, FF_PATH("w"), false);
        if (wf != NULL) {
            wf->wstream = axl_fopen(FF_PATH("w"), "w");
            wf->open    = (wf->wstream != NULL);
        }
        axl_ssize_t n = (wf != NULL && wf->wstream != NULL)
                      ? axl_write(wf->wstream, "bytes", 5) : -1;
        test_check(n == 5,
                   "9p clunk flush-fail: a write fid accepted 5 bytes");
        test_check(!s9p_fid_clunk(conn, 1),
                   "9p clunk flush-fail: clunking it reports the failed flush");
        test_check(s9p_fid_get(conn, 1) == NULL,
                   "9p clunk flush-fail: the fid is released even so");

        /* Control: the failure is the FLUSH's, not the clunk's. A fid with
           nothing open clunks clean on the very same volume. */
        test_check(s9p_fid_alloc(conn, 2, FF_PATH("w"), false) != NULL
                   && s9p_fid_clunk(conn, 2),
                   "9p clunk flush-fail: a fid with no write stream clunks clean");
    } else {
        test_check(false, "9p clunk flush-fail: server over the fixture volume");
        test_skip_n(3, "9p clunk flush-fail: status");
    }

    axl_9p_server_free(srv);
    axl_loop_free(loop);
    ff_fs_down();
}

/* axl_9p_negotiate_msize -- the Tversion msize decision, factored out of the
   server's handler so the floor/cap arithmetic is testable without a live
   socket. A client msize below AXL_9P_MIN_MSIZE must be REFUSED rather than
   answered (answering with a bigger msize than requested is forbidden by the
   spec, and answering with the client's nonsense value would leave the
   server's read/write chunking to underflow on msize - header). */
static void
test_9p_negotiate_msize(void)
{
    /* Mirrors AXL_9P_SERVER_MAX_MSIZE (private to axl-9p-server.c) purely as
       a realistic sample cap -- the helper's contract is "clamp to whatever
       cap you are handed", so nothing here breaks if the server picks a
       different ceiling. Kept in step by name so a reader can find it. */
    const uint32_t cap = 128u * 1024u;
    uint32_t out = 0xffffffffu;

    test_check(!axl_9p_negotiate_msize(0, cap, &out),
               "9p msize: a client msize of 0 is refused");
    test_check(out == 0xffffffffu, "9p msize: a refused negotiation leaves *out alone");
    test_check(!axl_9p_negotiate_msize(AXL_9P_MIN_MSIZE - 1, cap, &out),
               "9p msize: one byte below the floor is refused");
    test_check(axl_9p_negotiate_msize(AXL_9P_MIN_MSIZE, cap, &out)
               && out == AXL_9P_MIN_MSIZE,
               "9p msize: exactly the floor is granted unchanged");
    test_check(axl_9p_negotiate_msize(AXL_9P_MSIZE, cap, &out)
               && out == AXL_9P_MSIZE,
               "9p msize: a request under the server cap is granted as-is");
    test_check(axl_9p_negotiate_msize(0xFFFFFFFFu, cap, &out) && out == cap,
               "9p msize: a request over the server cap is clamped to the cap");
}

/* Axl9pServer arg-contract checks -- no live socket involved (axl_9p_server_new
   only allocates + copies the root string; a real axl_tcp_listen would need
   network bring-up, which is out of scope here -- full liveness is proven by
   the Task 6 integration test). */
static void
test_9p_server_arg_contract(void)
{
    AxlLoop *loop = axl_loop_new();
    test_check(loop != NULL, "9p server: test loop created");

    Axl9pServer *s = NULL;
    test_check(axl_9p_server_new(NULL, "fs0:\\", false, &s) == AXL_ERR,
               "9p server: new rejects NULL loop");
    test_check(axl_9p_server_new(loop, NULL, false, &s) == AXL_ERR,
               "9p server: new rejects NULL root");
    test_check(axl_9p_server_new(loop, "fs0:\\", false, NULL) == AXL_ERR,
               "9p server: new rejects NULL out");
    /* An empty root would silently defeat the path-safety design: with
       root == "", axl_path_join("", "foo") == "foo", dirname("foo") == ".",
       and the axl_strcmp(path, root) root-escape guard never matches. */
    test_check(axl_9p_server_new(loop, "", false, &s) == AXL_ERR,
               "9p server: new rejects an empty root");

    char long_root[300];
    axl_memset(long_root, 'a', sizeof(long_root) - 1);
    long_root[sizeof(long_root) - 1] = '\0';
    test_check(axl_9p_server_new(loop, long_root, false, &s) == AXL_ERR,
               "9p server: new rejects a root too long for the internal buffer");

    test_check(axl_9p_server_listen(NULL, 0) == AXL_ERR,
               "9p server: listen rejects NULL server");

    axl_9p_server_free(NULL);   /* NULL-safe no-op -- must not crash */

    Axl9pServer *srv = NULL;
    test_check(axl_9p_server_new(loop, "fs0:\\", false, &srv) == AXL_OK,
               "9p server: new succeeds with valid args");
    test_check(srv != NULL, "9p server: new populates *out");
    axl_9p_server_free(srv);

    axl_loop_free(loop);
}

static int
test_9p_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* See axl-test-vterm.c: without this header the harness cannot bracket
       this binary, so neither the stall detector nor the per-binary leak
       verdict check covers it. */
    test_print_header("Axl9p");

    test_codec_roundtrip();
    test_codec_byte_layout();
    test_codec_str_truncation_boundary();
    test_codec_overflow_and_underrun();
    test_9p_w_bytes();
    test_tversion_golden();
    test_9p_getattr_codec();
    test_parse_readdir_well_formed();
    test_parse_readdir_dir_entry();
    test_parse_readdir_eof();
    test_parse_readdir_dcount_lies();
    test_parse_readdir_truncated_entry();
    test_autoptr_9p_client();
    test_9p_server_reply_codec();
    test_9p_w_patch_u16();
    test_9p_w_patch_u32();
    test_9p_w_reserve();
    test_9p_read_clamp();
    test_9p_readdir_resume();
    test_9p_dirent_put();
    test_9p_path_up();
    test_9p_comp_is_safe();
    test_9p_type_is_mutating();
    test_9p_write_bounds();
    test_9p_write_grow_ok();
    test_9p_write_grow_needs_size();
    test_9p_setattr_size_ok();
    test_9p_view_coherence();
    test_9p_dir_is_empty();
    test_9p_fid_release_closes_open_state();
    test_9p_clunk_reports_a_failed_flush();
    test_9p_negotiate_msize();
    test_9p_server_arg_contract();
    return test_print_results();
}

AXL_APP(test_9p_main)
