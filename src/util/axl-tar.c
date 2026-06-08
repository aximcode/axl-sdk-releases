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

struct AxlTarReader {
    AxlStream  *in;         // borrowed
    uint64_t    remaining;  // unread data bytes of the current entry
    uint64_t    pad;        // padding bytes after the current entry's data
};

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
    for (int i = 0; i < AXL_TAR_BLOCK; i++) { sum += hdr[i]; }
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
    }
    return r;
}

int
axl_tar_reader_next(
    AxlTarReader  *r,
    AxlTarEntry   *out
    )
{
    if (r == NULL || out == NULL) { return AXL_ERR; }

    /* Skip any unread data + padding from the previous entry. */
    uint64_t skip = r->remaining + r->pad;
    while (skip > 0) {
        uint8_t tmp[AXL_TAR_BLOCK];
        size_t  chunk = (skip > sizeof tmp) ? sizeof tmp : (size_t)skip;
        axl_ssize_t n = axl_read(r->in, tmp, chunk);
        if (n <= 0) { break; }
        skip -= (uint64_t)n;
    }
    r->remaining = 0;
    r->pad = 0;

    uint8_t hdr[AXL_TAR_BLOCK];
    if (axl_read(r->in, hdr, AXL_TAR_BLOCK) != (axl_ssize_t)AXL_TAR_BLOCK) {
        return AXL_ERR;  // truncated / EOF before a full header
    }
    bool allzero = true;
    for (int i = 0; i < AXL_TAR_BLOCK; i++) {
        if (hdr[i] != 0) { allzero = false; break; }
    }
    if (allzero) { return AXL_ERR; }  // end-of-archive marker

    /* Verify the header checksum (chksum field treated as spaces). */
    uint64_t stored = get_octal((char *)hdr + 148, 8);
    unsigned sum = 0;
    for (int i = 0; i < AXL_TAR_BLOCK; i++) {
        sum += (i >= 148 && i < 156) ? (unsigned)' ' : hdr[i];
    }
    if ((uint64_t)sum != stored) { return AXL_ERR; }

    char name100[101];
    char prefix155[156];
    axl_memcpy(name100, hdr, 100);
    name100[100] = '\0';
    axl_memcpy(prefix155, hdr + 345, 155);
    prefix155[155] = '\0';

    axl_memset(out->name, 0, sizeof out->name);
    size_t nl = axl_strlen(name100);
    if (prefix155[0] != '\0') {
        size_t pl = axl_strlen(prefix155);
        if (pl + 1 + nl >= sizeof out->name) { return AXL_ERR; }
        axl_memcpy(out->name, prefix155, pl);
        out->name[pl] = '/';
        axl_memcpy(out->name + pl + 1, name100, nl);
    } else {
        if (nl >= sizeof out->name) { return AXL_ERR; }
        axl_memcpy(out->name, name100, nl);
    }

    out->size = get_octal((char *)hdr + 124, 12);
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
    if (n < 0) { return -1; }
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
