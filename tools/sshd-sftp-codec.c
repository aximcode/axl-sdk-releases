/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sshd-sftp-codec.c - SFTP version 3 wire codec and path normaliser. Pure;
 * see sshd-sftp-codec.h for the contract and AxlTestSshdSftp for the tests.
 */

#include "sshd-sftp-codec.h"

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/// Longest handle string a request may carry (ours are SFTP_HANDLE_LEN; a
/// longer one is still parsed, then not found).
#define SFTP_HANDLE_WIRE_MAX  256u

/// Half a year, as OpenSSH's ls_file() decides between time and year.
#define SFTP_HALF_YEAR_SEC    ((365u * 24u * 60u * 60u) / 2u)

/// The smallest room the framer allocates.
#define SFTP_RX_MIN_CAP       1024u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// A bounded reader over one packet.
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         off;
} Rd;

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static bool rd_u8(Rd *r, uint8_t *v);
static bool rd_u32(Rd *r, uint32_t *v);
static bool rd_u64(Rd *r, uint64_t *v);
static bool rd_str(Rd *r, SftpStr *s, uint32_t max);
static void put_bytes(SftpOut *o, const void *p, size_t n);
static void put_u8(SftpOut *o, uint8_t v);
static void put_u32(SftpOut *o, uint32_t v);
static void put_u64(SftpOut *o, uint64_t v);
static void put_str(SftpOut *o, const void *p, size_t n);
static void put_cstr(SftpOut *o, const char *s);
static void patch_u32(SftpOut *o, size_t at, uint32_t v);
static void reply_begin(SftpOut *o, uint8_t type, uint32_t id);
static void reply_end(SftpOut *o);
static void civil(uint64_t unix_secs, uint32_t *y, uint32_t *m, uint32_t *d,
                  uint32_t *hh, uint32_t *mm);
static bool path_bad_byte(uint8_t b);
static bool seg_push(char *canon, size_t *clen, const uint8_t *seg, size_t n);
static void seg_pop(char *canon, size_t *clen);

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

int
sftp_rx_feed(SftpRx *rx, const uint8_t *buf, size_t len)
{
    if (rx == NULL || (buf == NULL && len > 0)) {
        return AXL_INVALID;
    }
    /* Drop what was handed out first, so the bound counts held bytes. */
    if (rx->head > 0) {
        axl_memmove(rx->buf, rx->buf + rx->head, rx->len - rx->head);
        rx->len -= rx->head;
        rx->head = 0;
    }
    if (len > SFTP_RX_MAX - rx->len) {
        return AXL_NO_RESOURCES;
    }
    if (rx->len + len > rx->cap) {
        size_t cap = rx->cap < SFTP_RX_MIN_CAP ? SFTP_RX_MIN_CAP : rx->cap;
        while (cap < rx->len + len) {
            cap *= 2;
        }
        if (cap > SFTP_RX_MAX) {
            cap = SFTP_RX_MAX;
        }
        uint8_t *nb = axl_realloc(rx->buf, cap);
        if (nb == NULL) {
            return AXL_NO_RESOURCES;
        }
        rx->buf = nb;
        rx->cap = cap;
    }
    if (len > 0) {
        axl_memcpy(rx->buf + rx->len, buf, len);
        rx->len += len;
    }
    return AXL_OK;
}

int
sftp_rx_next(SftpRx *rx, const uint8_t **body, uint32_t *len)
{
    if (rx == NULL || body == NULL || len == NULL) {
        return AXL_INVALID;
    }
    size_t held = rx->len - rx->head;
    if (held < 4) {
        return AXL_INCOMPLETE;
    }
    const uint8_t *p = rx->buf + rx->head;
    uint32_t n = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                 | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (n == 0 || n > SFTP_PACKET_MAX) {
        return AXL_INVALID;
    }
    if (held - 4 < n) {
        return AXL_INCOMPLETE;
    }
    *body = p + 4;
    *len  = n;
    rx->head += 4u + (size_t)n;
    return AXL_OK;
}

void
sftp_rx_free(SftpRx *rx)
{
    if (rx != NULL) {
        axl_free(rx->buf);
        axl_memset(rx, 0, sizeof(*rx));
    }
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

static bool
rd_u8(Rd *r, uint8_t *v)
{
    if (r->len - r->off < 1) {
        return false;
    }
    *v = r->p[r->off++];
    return true;
}

static bool
rd_u32(Rd *r, uint32_t *v)
{
    if (r->len - r->off < 4) {
        return false;
    }
    const uint8_t *p = r->p + r->off;
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    r->off += 4;
    return true;
}

static bool
rd_u64(Rd *r, uint64_t *v)
{
    uint32_t hi = 0;
    uint32_t lo = 0;

    if (r->len - r->off < 8 || !rd_u32(r, &hi) || !rd_u32(r, &lo)) {
        return false;
    }
    *v = ((uint64_t)hi << 32) | lo;
    return true;
}

/* A string of at most @a max bytes, inside the packet. */
static bool
rd_str(Rd *r, SftpStr *s, uint32_t max)
{
    uint32_t n = 0;

    if (!rd_u32(r, &n) || n > max || r->len - r->off < n) {
        return false;
    }
    s->p   = r->p + r->off;
    s->len = n;
    r->off += n;
    return true;
}

int
sftp_attrs_decode(const uint8_t *p, size_t len, size_t *off, SftpAttrs *a)
{
    if (p == NULL || off == NULL || a == NULL || *off > len) {
        return AXL_INVALID;
    }
    Rd r = { p, len, *off };

    axl_memset(a, 0, sizeof(*a));
    if (!rd_u32(&r, &a->flags)) {
        return AXL_INVALID;
    }
    if ((a->flags & SFTP_ATTR_SIZE) != 0 && !rd_u64(&r, &a->size)) {
        return AXL_INVALID;
    }
    if ((a->flags & SFTP_ATTR_UIDGID) != 0
        && (!rd_u32(&r, &a->uid) || !rd_u32(&r, &a->gid))) {
        return AXL_INVALID;
    }
    if ((a->flags & SFTP_ATTR_PERMISSIONS) != 0 && !rd_u32(&r, &a->perm)) {
        return AXL_INVALID;
    }
    if ((a->flags & SFTP_ATTR_ACMODTIME) != 0
        && (!rd_u32(&r, &a->atime) || !rd_u32(&r, &a->mtime))) {
        return AXL_INVALID;
    }
    if ((a->flags & SFTP_ATTR_EXTENDED) != 0) {
        uint32_t n = 0;
        if (!rd_u32(&r, &n) || n > SFTP_ATTR_EXT_MAX) {
            return AXL_INVALID;
        }
        for (uint32_t i = 0; i < n; i++) {
            SftpStr k;
            SftpStr v;
            if (!rd_str(&r, &k, SFTP_PACKET_MAX) || !rd_str(&r, &v, SFTP_PACKET_MAX)) {
                return AXL_INVALID;
            }
        }
    }
    *off = r.off;
    return AXL_OK;
}

int
sftp_decode(const uint8_t *body, size_t len, SftpRequest *r)
{
    if (body == NULL || r == NULL) {
        return AXL_INVALID;
    }
    axl_memset(r, 0, sizeof(*r));
    Rd rd = { body, len, 0 };

    if (!rd_u8(&rd, &r->type)) {
        return AXL_INVALID;
    }
    if (r->type == SFTP_INIT) {
        return rd_u32(&rd, &r->version) ? AXL_OK : AXL_INVALID;
    }
    if (!rd_u32(&rd, &r->id)) {
        return AXL_INVALID;
    }
    bool ok = true;
    switch (r->type) {
    case SFTP_OPEN:
        ok = rd_str(&rd, &r->path, SFTP_PACKET_MAX) && rd_u32(&rd, &r->pflags);
        if (ok) {
            ok = sftp_attrs_decode(body, len, &rd.off, &r->attrs) == AXL_OK;
        }
        break;
    case SFTP_CLOSE:
    case SFTP_FSTAT:
    case SFTP_READDIR:
        ok = rd_str(&rd, &r->handle, SFTP_HANDLE_WIRE_MAX);
        break;
    case SFTP_READ:
        ok = rd_str(&rd, &r->handle, SFTP_HANDLE_WIRE_MAX)
             && rd_u64(&rd, &r->offset) && rd_u32(&rd, &r->length);
        break;
    case SFTP_WRITE:
        ok = rd_str(&rd, &r->handle, SFTP_HANDLE_WIRE_MAX)
             && rd_u64(&rd, &r->offset) && rd_str(&rd, &r->data, SFTP_PACKET_MAX);
        break;
    case SFTP_LSTAT:
    case SFTP_STAT:
    case SFTP_OPENDIR:
    case SFTP_REMOVE:
    case SFTP_RMDIR:
    case SFTP_REALPATH:
    case SFTP_READLINK:
        ok = rd_str(&rd, &r->path, SFTP_PACKET_MAX);
        break;
    case SFTP_SETSTAT:
    case SFTP_MKDIR:
        ok = rd_str(&rd, &r->path, SFTP_PACKET_MAX);
        if (ok) {
            ok = sftp_attrs_decode(body, len, &rd.off, &r->attrs) == AXL_OK;
        }
        break;
    case SFTP_FSETSTAT:
        ok = rd_str(&rd, &r->handle, SFTP_HANDLE_WIRE_MAX);
        if (ok) {
            ok = sftp_attrs_decode(body, len, &rd.off, &r->attrs) == AXL_OK;
        }
        break;
    case SFTP_RENAME:
    case SFTP_SYMLINK:
        ok = rd_str(&rd, &r->path, SFTP_PACKET_MAX)
             && rd_str(&rd, &r->path2, SFTP_PACKET_MAX);
        break;
    case SFTP_EXTENDED:
        ok = rd_str(&rd, &r->ext, SFTP_PACKET_MAX);
        if (ok) {
            r->data.p   = body + rd.off;
            r->data.len = (uint32_t)(len - rd.off);
        }
        break;
    default:
        /* Unknown: the caller answers it (sftp-server sends nothing). */
        break;
    }
    return ok ? AXL_OK : AXL_INVALID;
}

int
sftp_ext_two_paths(const SftpRequest *r, SftpStr *first, SftpStr *second)
{
    if (r == NULL || first == NULL || second == NULL || r->type != SFTP_EXTENDED
        || r->data.p == NULL) {
        return AXL_INVALID;
    }
    Rd rd = { r->data.p, r->data.len, 0 };

    return rd_str(&rd, first, SFTP_PACKET_MAX) && rd_str(&rd, second, SFTP_PACKET_MAX)
               ? AXL_OK
               : AXL_INVALID;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void
sftp_put_limits(SftpOut *o, uint32_t id, uint64_t packet, uint64_t read,
                uint64_t write, uint64_t handles)
{
    reply_begin(o, SFTP_EXTENDED_REPLY, id);
    put_u64(o, packet);
    put_u64(o, read);
    put_u64(o, write);
    put_u64(o, handles);
    reply_end(o);
}

void
sftp_out_init(SftpOut *o, void *storage, size_t cap)
{
    o->p   = storage;
    o->cap = storage != NULL ? cap : 0;
    o->len   = 0;
    o->start = 0;
    o->bad   = false;
}

static void
put_bytes(SftpOut *o, const void *p, size_t n)
{
    if (o->bad || n > o->cap - o->len) {
        o->bad = true;
        return;
    }
    if (n > 0) {
        axl_memcpy(o->p + o->len, p, n);
        o->len += n;
    }
}

static void
put_u8(SftpOut *o, uint8_t v)
{
    put_bytes(o, &v, 1);
}

static void
put_u32(SftpOut *o, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    put_bytes(o, b, sizeof(b));
}

static void
put_u64(SftpOut *o, uint64_t v)
{
    put_u32(o, (uint32_t)(v >> 32));
    put_u32(o, (uint32_t)v);
}

static void
put_str(SftpOut *o, const void *p, size_t n)
{
    if (n > UINT32_MAX) {
        o->bad = true;
        return;
    }
    put_u32(o, (uint32_t)n);
    put_bytes(o, p, n);
}

static void
put_cstr(SftpOut *o, const char *s)
{
    put_str(o, s, s != NULL ? axl_strlen(s) : 0);
}

static void
patch_u32(SftpOut *o, size_t at, uint32_t v)
{
    if (!o->bad && at + 4 <= o->len) {
        o->p[at]     = (uint8_t)(v >> 24);
        o->p[at + 1] = (uint8_t)(v >> 16);
        o->p[at + 2] = (uint8_t)(v >> 8);
        o->p[at + 3] = (uint8_t)v;
    }
}

/* Start a reply: its length is patched by reply_end. */
static void
reply_begin(SftpOut *o, uint8_t type, uint32_t id)
{
    o->start = o->len;
    put_u32(o, 0);
    put_u8(o, type);
    if (type != SFTP_VERSION_MSG) {
        put_u32(o, id);
    }
}

static void
reply_end(SftpOut *o)
{
    if (!o->bad) {
        patch_u32(o, o->start, (uint32_t)(o->len - o->start - 4u));
    }
}

size_t
sftp_out_reply_len(const SftpOut *o)
{
    if (o == NULL || o->bad) {
        return 0;
    }
    return o->len - o->start;
}

const char *
sftp_status_message(uint32_t code)
{
    static const char *const MSG[] = {
        "Success", "End of file", "No such file", "Permission denied",
        "Failure", "Bad message", "No connection", "Connection lost",
        "Operation unsupported",
    };

    return code < AXL_ARRAY_SIZE(MSG) ? MSG[code] : "Unknown error";
}

void
sftp_put_version(SftpOut *o, const char *const *ext_names,
                 const char *const *ext_data, size_t n)
{
    reply_begin(o, SFTP_VERSION_MSG, 0);
    put_u32(o, SFTP_VERSION);
    for (size_t i = 0; i < n; i++) {
        put_cstr(o, ext_names[i]);
        put_cstr(o, ext_data[i]);
    }
    reply_end(o);
}

void
sftp_put_status(SftpOut *o, uint32_t id, uint32_t code, const char *msg)
{
    reply_begin(o, SFTP_STATUS, id);
    put_u32(o, code);
    put_cstr(o, msg != NULL ? msg : sftp_status_message(code));
    put_cstr(o, "");
    reply_end(o);
}

void
sftp_put_handle(SftpOut *o, uint32_t id, const uint8_t *h, size_t len)
{
    reply_begin(o, SFTP_HANDLE, id);
    put_str(o, h, len);
    reply_end(o);
}

uint8_t *
sftp_data_begin(SftpOut *o, uint32_t id, size_t max)
{
    reply_begin(o, SFTP_DATA, id);
    put_u32(o, 0);
    if (o->bad || max > o->cap - o->len) {
        o->bad = true;
        return NULL;
    }
    return o->p + o->len;
}

void
sftp_data_end(SftpOut *o, size_t n)
{
    if (o->bad || n > o->cap - o->len) {
        o->bad = true;
        return;
    }
    patch_u32(o, o->len - 4, (uint32_t)n);
    o->len += n;
    reply_end(o);
}

void
sftp_attrs_encode(SftpOut *o, const SftpAttrs *a)
{
    put_u32(o, a->flags & ~SFTP_ATTR_EXTENDED);
    if ((a->flags & SFTP_ATTR_SIZE) != 0) {
        put_u64(o, a->size);
    }
    if ((a->flags & SFTP_ATTR_UIDGID) != 0) {
        put_u32(o, a->uid);
        put_u32(o, a->gid);
    }
    if ((a->flags & SFTP_ATTR_PERMISSIONS) != 0) {
        put_u32(o, a->perm);
    }
    if ((a->flags & SFTP_ATTR_ACMODTIME) != 0) {
        put_u32(o, a->atime);
        put_u32(o, a->mtime);
    }
}

void
sftp_put_attrs(SftpOut *o, uint32_t id, const SftpAttrs *a)
{
    reply_begin(o, SFTP_ATTRS, id);
    sftp_attrs_encode(o, a);
    reply_end(o);
}

void
sftp_name_begin(SftpOut *o, uint32_t id)
{
    reply_begin(o, SFTP_NAME, id);
    put_u32(o, 0);   /* the count, patched by sftp_name_end */
}

void
sftp_name_add(SftpOut *o, const char *name, const char *longname,
              const SftpAttrs *a)
{
    put_cstr(o, name);
    put_cstr(o, longname);
    sftp_attrs_encode(o, a);
}

void
sftp_name_end(SftpOut *o, uint32_t count)
{
    if (o->bad) {
        return;
    }
    /* length(4) type(1) id(4) count(4) */
    patch_u32(o, o->start + 9u, count);
    reply_end(o);
}

// ---------------------------------------------------------------------------
// Attributes and listings
// ---------------------------------------------------------------------------

void
sftp_attrs_synth(SftpAttrs *a, bool is_dir, bool read_only, uint64_t size,
                 uint64_t mtime_unix)
{
    uint32_t mode = is_dir ? (SFTP_S_IFDIR | 0755u) : (SFTP_S_IFREG | 0644u);
    uint32_t t    = mtime_unix > UINT32_MAX ? UINT32_MAX : (uint32_t)mtime_unix;

    if (read_only) {
        mode &= ~0222u;
    }
    axl_memset(a, 0, sizeof(*a));
    a->flags = SFTP_ATTR_SIZE | SFTP_ATTR_PERMISSIONS | SFTP_ATTR_ACMODTIME;
    a->size  = is_dir ? 0 : size;
    a->perm  = mode;
    a->atime = t;
    a->mtime = t;
}

/* Unix seconds to a UTC civil date (Howard Hinnant's days_from_civil,
   inverted). */
static void
civil(uint64_t unix_secs, uint32_t *y, uint32_t *m, uint32_t *d,
      uint32_t *hh, uint32_t *mm)
{
    uint64_t days = unix_secs / 86400u;
    uint64_t sod  = unix_secs % 86400u;
    uint64_t z    = days + 719468u;
    uint64_t era  = z / 146097u;
    uint64_t doe  = z - era * 146097u;
    uint64_t yoe  = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint64_t doy  = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint64_t mp   = (5u * doy + 2u) / 153u;

    *d  = (uint32_t)(doy - (153u * mp + 2u) / 5u + 1u);
    *m  = (uint32_t)(mp < 10u ? mp + 3u : mp - 9u);
    *y  = (uint32_t)(yoe + era * 400u + (*m <= 2u ? 1u : 0u));
    *hh = (uint32_t)(sod / 3600u);
    *mm = (uint32_t)((sod % 3600u) / 60u);
}

void
sftp_longname(char *out, size_t cap, const char *name, const SftpAttrs *a,
              uint64_t now_unix)
{
    static const char *const MON[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char     mode[11];
    char     when[16];
    uint32_t p = a->perm;
    uint32_t y = 0, mo = 0, d = 0, hh = 0, mi = 0;

    if (out == NULL || cap == 0) {
        return;
    }
    mode[0]  = (p & SFTP_S_IFDIR) == SFTP_S_IFDIR ? 'd' : '-';
    mode[1]  = (p & 0400u) != 0 ? 'r' : '-';
    mode[2]  = (p & 0200u) != 0 ? 'w' : '-';
    mode[3]  = (p & 0100u) != 0 ? 'x' : '-';
    mode[4]  = (p & 0040u) != 0 ? 'r' : '-';
    mode[5]  = (p & 0020u) != 0 ? 'w' : '-';
    mode[6]  = (p & 0010u) != 0 ? 'x' : '-';
    mode[7]  = (p & 0004u) != 0 ? 'r' : '-';
    mode[8]  = (p & 0002u) != 0 ? 'w' : '-';
    mode[9]  = (p & 0001u) != 0 ? 'x' : '-';
    mode[10] = '\0';

    uint64_t t = a->mtime;
    civil(t, &y, &mo, &d, &hh, &mi);
    if (now_unix - SFTP_HALF_YEAR_SEC < t && now_unix >= t) {
        axl_snprintf(when, sizeof(when), "%s %2u %02u:%02u", MON[mo - 1u],
                     (unsigned)d, (unsigned)hh, (unsigned)mi);
    } else {
        axl_snprintf(when, sizeof(when), "%s %2u  %u", MON[mo - 1u],
                     (unsigned)d, (unsigned)y);
    }
    /* strmode()'s 11th character is a space: hence two before the count. */
    axl_snprintf(out, cap, "%s  %3u %-8s %-8s %8llu %s %s", mode, 1u, "0", "0",
                 (unsigned long long)a->size, when, name);
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

void
sftp_handle_encode(uint8_t out[SFTP_HANDLE_LEN], uint32_t index, uint32_t gen)
{
    out[0] = (uint8_t)(index >> 24);
    out[1] = (uint8_t)(index >> 16);
    out[2] = (uint8_t)(index >> 8);
    out[3] = (uint8_t)index;
    out[4] = (uint8_t)(gen >> 24);
    out[5] = (uint8_t)(gen >> 16);
    out[6] = (uint8_t)(gen >> 8);
    out[7] = (uint8_t)gen;
}

bool
sftp_handle_decode(SftpStr h, uint32_t *index, uint32_t *gen)
{
    if (h.p == NULL || h.len != SFTP_HANDLE_LEN || index == NULL || gen == NULL) {
        return false;
    }
    *index = ((uint32_t)h.p[0] << 24) | ((uint32_t)h.p[1] << 16)
             | ((uint32_t)h.p[2] << 8) | (uint32_t)h.p[3];
    *gen   = ((uint32_t)h.p[4] << 24) | ((uint32_t)h.p[5] << 16)
             | ((uint32_t)h.p[6] << 8) | (uint32_t)h.p[7];
    return true;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static bool
path_bad_byte(uint8_t b)
{
    return b < 0x20u || b == 0x7fu;
}

/* Append "/<seg>" to the canonical form. False when it does not fit. */
static bool
seg_push(char *canon, size_t *clen, const uint8_t *seg, size_t n)
{
    if (*clen + 1u + n >= SFTP_PATH_MAX) {
        return false;
    }
    canon[(*clen)++] = '/';
    axl_memcpy(canon + *clen, seg, n);
    *clen += n;
    canon[*clen] = '\0';
    return true;
}

/* Remove the last "/<seg>"; at the root, nothing. */
static void
seg_pop(char *canon, size_t *clen)
{
    while (*clen > 0 && canon[*clen - 1u] != '/') {
        (*clen)--;
    }
    if (*clen > 0) {
        (*clen)--;
    }
    canon[*clen] = '\0';
}

int
sftp_path_normalize(const uint8_t *in, size_t len, SftpPath *out)
{
    if ((in == NULL && len > 0) || out == NULL) {
        return AXL_INVALID;
    }
    if (len >= SFTP_PATH_MAX) {
        return AXL_NO_RESOURCES;
    }
    for (size_t i = 0; i < len; i++) {
        if (path_bad_byte(in[i])) {
            return AXL_INVALID;
        }
    }
    char   *canon = out->canon;
    size_t  clen  = 0;
    size_t  i     = 0;

    canon[0] = '\0';

    /* The Shell form: a first component (no leading separator) holding a
       colon names the volume before it; the rest is relative to it. */
    if (len > 0 && in[0] != '/' && in[0] != '\\') {
        size_t c = 0;
        while (c < len && in[c] != ':' && in[c] != '/' && in[c] != '\\') {
            c++;
        }
        if (c < len && in[c] == ':') {
            if (c == 0) {
                return AXL_INVALID;
            }
            if (!seg_push(canon, &clen, in, c)) {
                return AXL_NO_RESOURCES;
            }
            i = c + 1u;
        }
    }
    bool first = clen == 0;
    while (i < len) {
        while (i < len && (in[i] == '/' || in[i] == '\\')) {
            i++;
        }
        size_t s = i;
        while (i < len && in[i] != '/' && in[i] != '\\') {
            i++;
        }
        size_t n = i - s;
        if (n == 0 || (n == 1 && in[s] == '.')) {
            continue;
        }
        if (n == 2 && in[s] == '.' && in[s + 1u] == '.') {
            seg_pop(canon, &clen);
            first = clen == 0;
            continue;
        }
        /* A canonical path's volume may still carry the Shell's colon
           ("/fs0:/a"): drop it. */
        if (first && in[s + n - 1u] == ':') {
            n--;
            if (n == 0) {
                return AXL_INVALID;
            }
        }
        if (!seg_push(canon, &clen, in + s, n)) {
            return AXL_NO_RESOURCES;
        }
        first = false;
    }
    if (clen == 0) {
        axl_strlcpy(canon, "/", SFTP_PATH_MAX);
        out->fs[0]     = '\0';
        out->volume[0] = '\0';
        return AXL_OK;
    }

    /* The volume: lower-cased in the canonical form too. */
    size_t vlen = 1;
    while (vlen < clen && canon[vlen] != '/') {
        vlen++;
    }
    vlen -= 1u;
    if (vlen >= SFTP_VOLUME_MAX) {
        return AXL_INVALID;
    }
    for (size_t k = 1; k <= vlen; k++) {
        char ch = canon[k];
        if (ch >= 'A' && ch <= 'Z') {
            canon[k] = (char)(ch - 'A' + 'a');
        }
    }
    axl_memcpy(out->volume, canon + 1, vlen);
    out->volume[vlen] = '\0';

    /* AxlFs's form: "vol:\rest\with\backslashes", "vol:\" for the root. */
    size_t rest = clen - 1u - vlen;          /* "/a/b" after the volume */
    size_t need = vlen + 2u + (rest > 0 ? rest - 1u : 0u);
    if (need >= SFTP_PATH_MAX) {
        return AXL_NO_RESOURCES;
    }
    char *fs = out->fs;
    axl_memcpy(fs, out->volume, vlen);
    fs[vlen]      = ':';
    fs[vlen + 1u] = '\\';
    size_t f = vlen + 2u;
    for (size_t k = vlen + 2u; k < clen; k++) {
        fs[f++] = canon[k] == '/' ? '\\' : canon[k];
    }
    fs[f] = '\0';
    return AXL_OK;
}
