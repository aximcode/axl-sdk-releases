/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tar.c
    POSIX ustar reader/writer over AxlStream — see <axl/axl-tar.h>.

    ustar header layout (offsets): name[0..99], mode[100..107],
    uid[108..115], gid[116..123], size[124..135], mtime[136..147],
    chksum[148..155], typeflag[156], linkname[157..256],
    magic[257..262]="ustar\0", version[263..264]="00", uname[265..296],
    gname[297..328], devmajor[329..336], devminor[337..344],
    prefix[345..499]. Numeric fields are NUL/space-terminated octal.
    Each header + each entry's data is padded to AXL_TAR_BLOCK.
**/

#include <axl/axl-tar.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

#include <stdbool.h>

struct AxlTarWriter {
    AxlStream  *out;       // borrowed
    uint64_t    written;   // total bytes emitted, for end-of-archive record padding
};

/* GNU/BSD tar's default blocking factor: archives are padded to a
   multiple of 20 * 512 = 10240 bytes. Without this, GNU tar reading a
   *streamed* archive (e.g. `tar -tzf`, where it can't stat the end)
   hits EOF mid-record after the two zero blocks and exits non-zero
   with a warning — even though it read every member. Padding to the
   record boundary makes streamed reads exit cleanly. */
#define AXL_TAR_RECORD  (20u * AXL_TAR_BLOCK)

/* Cap on the data of a GNU 'L'/'K' or pax 'x' record the reader holds in
   memory: far above any name it can report. A larger record is stepped
   over, and the member it describes is refused. */
#define TAR_META_MAX  (64u * 1024u)

/* Largest member size a pax `size` may give: off_t's maximum, as GNU tar
   reads it. Far beyond any real file, and it keeps size + padding (at
   most 511) from wrapping a uint64_t -- a wrapped skip of 0 would parse
   the member's own DATA as the next header. A larger value refuses the
   member as a malformed record set. */
#define TAR_SIZE_MAX  ((uint64_t)INT64_MAX)

struct AxlTarReader {
    AxlStream  *in;         // borrowed
    uint64_t    remaining;  // unread data bytes of the current entry
    uint64_t    pad;        // padding bytes after the current entry's data
    int         stopped;    // AXL_OK while reading; else the status every
                            //   later axl_tar_reader_next returns
};

/* What GNU 'L' and pax 'x' records say about the member that follows. */
typedef struct {
    bool      have_name;               // an 'L' or a pax path named it
    bool      refused;                 // ...with a name not reported whole
    char      name[AXL_TAR_NAME_MAX];
    bool      have_size;               // a pax size overrides the header's
    uint64_t  size;
} TarPending;

static int   tar_read_status(axl_ssize_t got);
static int   tar_skip(AxlStream *in, uint64_t n);
static int   tar_read_full(AxlStream *in, void *buf, size_t n, size_t *done);
static int   tar_read_meta(AxlStream *in, uint64_t size, char **out);
static void  tar_set_name(TarPending *pend, const char *s, size_t n);
static void  tar_refuse(TarPending *pend, bool untrusted);
static bool  tar_field_len(const uint8_t *f, size_t cap, size_t *len);
static void  tar_apply_gnu_long(TarPending *pend, const char *data, uint64_t n);
static void  tar_apply_pax(TarPending *pend, const char *data, uint64_t n);
static int   tar_next(AxlTarReader *r, AxlTarEntry *out);

// ---------------------------------------------------------------------------
// Octal field helpers
// ---------------------------------------------------------------------------

/* Write @p val as (fieldlen-1) zero-padded octal digits + NUL into a
   fieldlen-byte field. High bits beyond the field are dropped (the
   caller keeps values within the ustar field width). */
static void
put_octal(
    char     *field,
    int       fieldlen,
    uint64_t  val
    )
{
    for (int i = fieldlen - 2; i >= 0; i--) {
        field[i] = (char)('0' + (int)(val & 7));
        val >>= 3;
    }
    field[fieldlen - 1] = '\0';
}

/* Parse an octal numeric field: skip leading spaces, accumulate octal
   digits, stop at the first non-octal byte (NUL or space). */
static uint64_t
get_octal(
    const char *field,
    int         fieldlen
    )
{
    int i = 0;
    while (i < fieldlen && field[i] == ' ') { i++; }
    uint64_t v = 0;
    while (i < fieldlen && field[i] >= '0' && field[i] <= '7') {
        v = (v << 3) | (uint64_t)(field[i] - '0');
        i++;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Name splitting (ustar name[100] + prefix[155])
// ---------------------------------------------------------------------------

/* Split @p path across the 100-byte name and 155-byte prefix fields.
   Both buffers are pre-zeroed here. Returns 0 on success, -1 if the
   path can't fit (no '/' yields a name part <= 100 with prefix <= 155). */
static int
split_name(
    const char *path,
    char       *name100,   // 100-byte field
    char       *prefix155  // 155-byte field
    )
{
    axl_memset(name100, 0, 100);
    axl_memset(prefix155, 0, 155);
    size_t len = axl_strlen(path);
    if (len == 0) { return -1; }
    if (len <= 100) {
        axl_memcpy(name100, path, len);
        return 0;
    }
    /* Find the earliest '/' whose suffix fits name[100] and whose prefix
       fits prefix[155]. As the split index grows the suffix shrinks, so
       the first qualifying '/' gives the shortest prefix. */
    for (size_t i = 0; i + 1 < len; i++) {
        if (path[i] != '/') { continue; }
        size_t prefix_len = i;
        size_t name_len   = len - i - 1;
        if (prefix_len <= 155 && name_len > 0 && name_len <= 100) {
            axl_memcpy(prefix155, path, prefix_len);
            axl_memcpy(name100, path + i + 1, name_len);
            return 0;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

AxlTarWriter *
axl_tar_writer_new(
    AxlStream  *out
    )
{
    if (out == NULL) { return NULL; }
    AxlTarWriter *w = axl_malloc(sizeof *w);
    if (w != NULL) { w->out = out; w->written = 0; }
    return w;
}

static int
write_header(
    AxlStream  *out,
    const char *path,
    uint32_t    mode,
    uint64_t    size,
    char        type
    )
{
    uint8_t hdr[AXL_TAR_BLOCK];
    axl_memset(hdr, 0, sizeof hdr);

    if (split_name(path, (char *)hdr + 0, (char *)hdr + 345) != 0) {
        return AXL_ERR;
    }
    put_octal((char *)hdr + 100, 8, mode & 07777u);  // mode
    put_octal((char *)hdr + 108, 8, 0);              // uid
    put_octal((char *)hdr + 116, 8, 0);              // gid
    put_octal((char *)hdr + 124, 12, size);          // size
    put_octal((char *)hdr + 136, 12, 0);             // mtime
    hdr[156] = (uint8_t)type;                        // typeflag
    axl_memcpy(hdr + 257, "ustar", 5);               // magic "ustar\0"
    hdr[262] = '\0';
    hdr[263] = '0';                                  // version "00"
    hdr[264] = '0';

    /* Checksum: sum every byte with the 8-byte chksum field read as
       spaces, then write it as 6 octal digits + NUL + space. */
    axl_memset(hdr + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned i = 0; i < AXL_TAR_BLOCK; i++) { sum += hdr[i]; }
    put_octal((char *)hdr + 148, 7, sum);  // 6 digits + NUL at [154]
    hdr[155] = ' ';

    return (axl_write(out, hdr, AXL_TAR_BLOCK) == (axl_ssize_t)AXL_TAR_BLOCK)
           ? AXL_OK : AXL_ERR;
}

/* Write @p len zero-padding bytes to round the last data block up. */
static int
write_padding(
    AxlStream  *out,
    uint64_t    len
    )
{
    size_t pad = (size_t)((AXL_TAR_BLOCK - (len % AXL_TAR_BLOCK)) % AXL_TAR_BLOCK);
    if (pad == 0) { return AXL_OK; }
    uint8_t zeros[AXL_TAR_BLOCK];
    axl_memset(zeros, 0, pad);
    return (axl_write(out, zeros, pad) == (axl_ssize_t)pad) ? AXL_OK : AXL_ERR;
}

int
axl_tar_writer_add(
    AxlTarWriter  *w,
    const char    *name,
    uint32_t       mode,
    const void    *data,
    size_t         len
    )
{
    if (w == NULL || name == NULL || (data == NULL && len > 0)) {
        return AXL_ERR;
    }
    if (write_header(w->out, name, mode, len, AXL_TAR_TYPE_FILE) != AXL_OK) {
        return AXL_ERR;
    }
    w->written += AXL_TAR_BLOCK;
    if (len > 0) {
        if (axl_write(w->out, data, len) != (axl_ssize_t)len) {
            return AXL_ERR;
        }
        if (write_padding(w->out, len) != AXL_OK) {
            return AXL_ERR;
        }
        w->written += len + (AXL_TAR_BLOCK - (len % AXL_TAR_BLOCK)) % AXL_TAR_BLOCK;
    }
    return AXL_OK;
}

int
axl_tar_writer_add_dir(
    AxlTarWriter  *w,
    const char    *name,
    uint32_t       mode
    )
{
    if (w == NULL || name == NULL) { return AXL_ERR; }
    size_t len = axl_strlen(name);
    char   tmp[AXL_TAR_NAME_MAX];
    if (len + 2 > sizeof tmp) { return AXL_ERR; }
    axl_memcpy(tmp, name, len);
    if (len == 0 || name[len - 1] != '/') { tmp[len++] = '/'; }
    tmp[len] = '\0';
    if (write_header(w->out, tmp, mode, 0, AXL_TAR_TYPE_DIR) != AXL_OK) {
        return AXL_ERR;
    }
    w->written += AXL_TAR_BLOCK;
    return AXL_OK;
}

int
axl_tar_writer_finish(
    AxlTarWriter  *w
    )
{
    if (w == NULL) { return AXL_ERR; }
    uint8_t zeros[AXL_TAR_BLOCK];
    axl_memset(zeros, 0, sizeof zeros);
    /* Two zero blocks mark end-of-archive (POSIX minimum). */
    for (int i = 0; i < 2; i++) {
        if (axl_write(w->out, zeros, AXL_TAR_BLOCK) != (axl_ssize_t)AXL_TAR_BLOCK) {
            return AXL_ERR;
        }
        w->written += AXL_TAR_BLOCK;
    }
    /* Pad to the 10240-byte record boundary so GNU tar reading the
       archive as a stream (tar -tzf / -xzf over gzip) exits cleanly. */
    uint64_t rem = w->written % AXL_TAR_RECORD;
    uint64_t recpad = (rem == 0) ? 0 : (AXL_TAR_RECORD - rem);
    while (recpad > 0) {
        size_t chunk = (recpad < AXL_TAR_BLOCK) ? (size_t)recpad : AXL_TAR_BLOCK;
        if (axl_write(w->out, zeros, chunk) != (axl_ssize_t)chunk) {
            return AXL_ERR;
        }
        w->written += chunk;
        recpad -= chunk;
    }
    return AXL_OK;
}

void
axl_tar_writer_free(
    AxlTarWriter  *w
    )
{
    axl_free(w);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

AxlTarReader *
axl_tar_reader_new(
    AxlStream  *in
    )
{
    if (in == NULL) { return NULL; }
    AxlTarReader *r = axl_malloc(sizeof *r);
    if (r != NULL) {
        r->in = in;
        r->remaining = 0;
        r->pad = 0;
        r->stopped = AXL_OK;
    }
    return r;
}

/* The status for an axl_read that returned @p got <= 0 while bytes were
   still owed: the stream ended early (truncated), or failed. */
static int
tar_read_status(
    axl_ssize_t  got
    )
{
    return (got == 0) ? AXL_INCOMPLETE : AXL_IO_ERROR;
}

/* Step over @p n bytes of @p in. AXL_INCOMPLETE if the stream ends first,
   AXL_IO_ERROR on a stream error. */
static int
tar_skip(
    AxlStream  *in,
    uint64_t    n
    )
{
    uint8_t tmp[AXL_TAR_BLOCK];
    while (n > 0) {
        size_t      chunk = (n > sizeof tmp) ? sizeof tmp : (size_t)n;
        axl_ssize_t got   = axl_read(in, tmp, chunk);
        if (got <= 0) { return tar_read_status(got); }
        n -= (uint64_t)got;
    }
    return AXL_OK;
}

/* Read exactly @p n bytes, counting them in @p *done. AXL_INCOMPLETE if
   the stream ends first, AXL_IO_ERROR on a stream error. */
static int
tar_read_full(
    AxlStream  *in,
    void       *buf,
    size_t      n,
    size_t     *done
    )
{
    *done = 0;
    while (*done < n) {
        axl_ssize_t got = axl_read(in, (uint8_t *)buf + *done, n - *done);
        if (got <= 0) { return tar_read_status(got); }
        *done += (size_t)got;
    }
    return AXL_OK;
}

/* Read a metadata record's @p size data bytes and their padding. Sets
   @p *out to a NUL-terminated heap copy, or to NULL when the record is
   over TAR_META_MAX (stepped over, not held) or memory runs out. Returns
   AXL_INCOMPLETE or AXL_IO_ERROR when the stream ends early or fails. */
static int
tar_read_meta(
    AxlStream  *in,
    uint64_t    size,
    char      **out
    )
{
    uint64_t pad = (AXL_TAR_BLOCK - (size % AXL_TAR_BLOCK)) % AXL_TAR_BLOCK;
    *out = NULL;
    char *buf = (size <= TAR_META_MAX) ? axl_malloc((size_t)size + 1) : NULL;
    if (buf == NULL) {
        return tar_skip(in, size + pad);
    }
    size_t done = 0;
    int    rc   = tar_read_full(in, buf, (size_t)size, &done);
    if (rc == AXL_OK) { rc = tar_skip(in, pad); }
    if (rc != AXL_OK) {
        axl_free(buf);
        return rc;
    }
    buf[size] = '\0';
    *out = buf;
    return AXL_OK;
}

/* Name the pending member @p s (@p n bytes), or refuse it when the name is
   empty, holds a NUL, or does not fit AxlTarEntry.name whole. */
static void
tar_set_name(
    TarPending  *pend,
    const char  *s,
    size_t       n
    )
{
    pend->have_name = true;
    pend->refused   = (n == 0 || n >= sizeof pend->name
                       || axl_memchr(s, '\0', n) != NULL);
    if (!pend->refused) {
        axl_memcpy(pend->name, s, n);
        pend->name[n] = '\0';
    }
}

/* Refuse the pending member. An @p untrusted record set (malformed, or
   too large to hold) also drops any size it gave, so the member's data
   is stepped over by its header size -- what a reader that ignores the
   record would use; a well-formed set refused only for its name keeps
   its size, which is the member's real length. */
static void
tar_refuse(
    TarPending  *pend,
    bool         untrusted
    )
{
    pend->have_name = true;
    pend->refused   = true;
    if (untrusted) {
        pend->have_size = false;
        pend->size      = 0;
    }
}

/* The length of a NUL-padded header name field of @p cap bytes. False
   when bytes follow the first NUL: such a name would be cut there. */
static bool
tar_field_len(
    const uint8_t  *f,
    size_t          cap,
    size_t         *len
    )
{
    size_t n = 0;
    while (n < cap && f[n] != 0) { n++; }
    for (size_t i = n; i < cap; i++) {
        if (f[i] != 0) { return false; }
    }
    *len = n;
    return true;
}

/* A GNU 'L' record: the name, NUL-terminated, then NUL padding. NULL
   @p data is a record too large to hold. */
static void
tar_apply_gnu_long(
    TarPending  *pend,
    const char  *data,
    uint64_t     n
    )
{
    size_t len = 0;
    if (data == NULL || !tar_field_len((const uint8_t *)data, (size_t)n, &len)) {
        tar_refuse(pend, false);
        return;
    }
    tar_set_name(pend, data, len);
}

/* A pax 'x' record set: "LEN key=value\n" records, LEN counting the whole
   record. `path` and `size` are honoured; other keys are ignored. A
   malformed set (or NULL @p data, one too large to hold), or a size over
   TAR_SIZE_MAX, refuses the member: its name and size cannot be trusted,
   so an earlier record's size is dropped too. */
static void
tar_apply_pax(
    TarPending  *pend,
    const char  *data,
    uint64_t     n
    )
{
    if (data == NULL) {
        tar_refuse(pend, true);
        return;
    }
    size_t pos = 0;
    while (pos < n) {
        size_t len = 0;
        size_t i   = pos;
        while (i < n && data[i] >= '0' && data[i] <= '9' && len <= n) {
            len = len * 10 + (size_t)(data[i] - '0');
            i++;
        }
        if (i >= n || data[i] != ' ' || len == 0 || len > n - pos
            || data[pos + len - 1] != '\n') {
            tar_refuse(pend, true);
            return;
        }
        const char *key = data + i + 1;
        const char *end = data + pos + len - 1;        /* the '\n' */
        const char *eq  = key;
        while (eq < end && *eq != '=') { eq++; }
        if (eq == end) {
            tar_refuse(pend, true);
            return;
        }
        size_t      klen = (size_t)(eq - key);
        const char *val  = eq + 1;
        size_t      vlen = (size_t)(end - val);
        if (klen == 4 && axl_memcmp(key, "path", 4) == 0) {
            tar_set_name(pend, val, vlen);
        } else if (klen == 4 && axl_memcmp(key, "size", 4) == 0) {
            uint64_t v  = 0;
            bool     ok = (vlen > 0);
            for (size_t k = 0; k < vlen && ok; k++) {
                ok = (val[k] >= '0' && val[k] <= '9'
                      && v <= (UINT64_MAX - 9u) / 10u);
                v = v * 10u + (uint64_t)(val[k] - '0');
            }
            if (!ok || v > TAR_SIZE_MAX) {
                tar_refuse(pend, true);
                return;
            }
            pend->have_size = true;
            pend->size      = v;
        }
        pos += len;
    }
}

int
axl_tar_reader_next(
    AxlTarReader  *r,
    AxlTarEntry   *out
    )
{
    if (r == NULL || out == NULL) { return AXL_ERR; }
    if (r->stopped != AXL_OK) { return r->stopped; }
    int rc = tar_next(r, out);
    if (rc != AXL_OK) { r->stopped = rc; }
    return rc;
}

/* axl_tar_reader_next's body, without the sticky stop. */
static int
tar_next(
    AxlTarReader  *r,
    AxlTarEntry   *out
    )
{
    TarPending pend;
    axl_memset(&pend, 0, sizeof pend);
    /* An 'L', 'K' or 'x' record was read: it announces a member, so the
       stream ending before that member's header is a truncation -- even a
       'K' or an 'x' of ignored keys, which leave nothing in @c pend. */
    bool record_pending = false;
    uint8_t  hdr[AXL_TAR_BLOCK];
    uint64_t size;
    for (;;) {
        /* Step over any unread data + padding of the previous entry: the
           stream ending in there is a truncated archive. */
        int rc = tar_skip(r->in, r->remaining + r->pad);
        if (rc != AXL_OK) { return rc; }
        r->remaining = 0;
        r->pad = 0;

        /* The stream ending exactly at a header boundary is an end, as GNU
           tar reads it; ending part-way into a header is a truncation (GNU
           tar accepts that silently and loses the member; libarchive
           reports it, and so do we). */
        size_t got = 0;
        rc = tar_read_full(r->in, hdr, AXL_TAR_BLOCK, &got);
        if (rc == AXL_INCOMPLETE && got == 0 && !record_pending) {
            return AXL_ERR;
        }
        if (rc != AXL_OK) { return rc; }
        bool allzero = true;
        for (unsigned i = 0; i < AXL_TAR_BLOCK; i++) {
            if (hdr[i] != 0) { allzero = false; break; }
        }
        if (allzero) { return AXL_ERR; }  // end-of-archive marker (one is enough)

        /* Verify the header checksum (chksum field treated as spaces). */
        uint64_t stored = get_octal((char *)hdr + 148, 8);
        unsigned sum = 0;
        for (unsigned i = 0; i < AXL_TAR_BLOCK; i++) {
            sum += (i >= 148 && i < 156) ? (unsigned)' ' : hdr[i];
        }
        if ((uint64_t)sum != stored) { return AXL_INVALID; }

        size = get_octal((char *)hdr + 124, 12);
        char type = (char)hdr[156];
        if (type != 'L' && type != 'K' && type != 'x') { break; }

        /* A record about the NEXT header: GNU long name / long link name,
           or pax extended attributes. Consume it and read on. */
        record_pending = true;
        char *data = NULL;
        rc = tar_read_meta(r->in, size, &data);
        if (rc != AXL_OK) { return rc; }
        if (type == 'L') {
            tar_apply_gnu_long(&pend, data, size);
        } else if (type == 'x') {
            tar_apply_pax(&pend, data, size);
        }   /* 'K' names a link target: no link is created, so drop it */
        axl_free(data);
    }

    /* A pax global header names no member: a pending 'x' is not its (a
       pending size would skip the wrong span), and pax writers never put
       one there. Drop it. */
    if ((char)hdr[156] == AXL_TAR_TYPE_GLOBAL) {
        axl_memset(&pend, 0, sizeof pend);
    }

    axl_memset(out, 0, sizeof *out);
    if (pend.have_name) {
        out->name_refused = pend.refused;
        if (!pend.refused) {
            axl_memcpy(out->name, pend.name, sizeof out->name);
        }
    } else {
        /* Only a POSIX header has a prefix: GNU's "ustar  " header keeps
           atime/ctime at the same offset. */
        bool   posix = (axl_memcmp(hdr + 257, "ustar\0", 6) == 0);
        size_t nl = 0;
        size_t pl = 0;
        if (!tar_field_len(hdr, 100, &nl)
            || (posix && !tar_field_len(hdr + 345, 155, &pl))) {
            out->name_refused = true;
        } else if (pl > 0) {
            /* 155 + '/' + 100 = 256: always fits AXL_TAR_NAME_MAX. */
            axl_memcpy(out->name, hdr + 345, pl);
            out->name[pl] = '/';
            axl_memcpy(out->name + pl + 1, hdr, nl);
        } else {
            axl_memcpy(out->name, hdr, nl);
        }
    }

    out->size = pend.have_size ? pend.size : size;
    out->mode = (uint32_t)get_octal((char *)hdr + 100, 8);
    out->type = (char)hdr[156];
    if (out->type == '\0') { out->type = AXL_TAR_TYPE_FILE; }

    r->remaining = out->size;
    r->pad = (AXL_TAR_BLOCK - (out->size % AXL_TAR_BLOCK)) % AXL_TAR_BLOCK;
    return AXL_OK;
}

axl_ssize_t
axl_tar_reader_read(
    AxlTarReader  *r,
    void          *buf,
    size_t         len
    )
{
    if (r == NULL || buf == NULL) { return -1; }
    if (r->remaining == 0) { return 0; }
    size_t want = (len > r->remaining) ? (size_t)r->remaining : len;
    axl_ssize_t n = axl_read(r->in, buf, want);
    if (n <= 0) {   /* EOF with bytes owed (a truncated archive), or an error */
        r->stopped = tar_read_status(n);
        return -1;
    }
    r->remaining -= (uint64_t)n;
    return n;
}

void
axl_tar_reader_free(
    AxlTarReader  *r
    )
{
    axl_free(r);
}
