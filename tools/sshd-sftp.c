/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sshd-sftp.c - the SFTP server behind sshd's `sftp` subsystem: one session
 * per channel, every version-3 operation over AxlFs. It implements the
 * SftpIface table (sshd-sftp-iface.h) and depends on nothing of sshd's: it
 * is linked into SftpServer.efi (sftp-server.c), which sshd loads as it starts.
 *
 * Semantics are OpenSSH sftp-server's (draft-ietf-secsh-filexfer-02,
 * version 3): one reply per request, in request order; status codes and
 * texts as sftp-server sends them for the same condition, its errno mapped
 * the way its errno_to_portable() maps it; a request that does not parse, or
 * a packet over SFTP_PACKET_MAX, ends the session; an unknown request type is
 * logged and not answered.
 *
 * Paths: `/` is a virtual directory of the mapped volumes (`fs0`, `fs1`,
 * ...); `/fs0/dir/file` and the Shell's `fs0:\dir\file` both name a file
 * (sftp_path_normalize()). Replies always use the `/fs0/...` form. For the
 * write operations `/` behaves as a directory the user does not own, and
 * each volume as a mount point in it: a new name in `/` is
 * PERMISSION_DENIED, a volume cannot be removed or renamed, and `/` itself
 * is FAILURE (sftp-server's EACCES, and EBUSY/EEXIST/EISDIR).
 *
 * Read-only mode (SFTP_SESSION_READ_ONLY) is enforced in dispatch() alone,
 * from the class column of the one table it dispatches by, so an operation
 * added to the table cannot forget it.
 *
 * Output: each request's reply is encoded whole into the session's reply
 * buffer, then written in pieces no larger than the channel accepts. A
 * piece refused with AXL_BUSY stops the session reading requests until the
 * writable call; requests that arrive meanwhile wait, framed, in the input
 * buffer.
 *
 * Input is paced by the SSH window: each request's packet is reported
 * consumed (SftpConsumedFn) as it is dispatched, and sshd runs the channel
 * in AXL_SSH_CHANNEL_WINDOW_ON_CONSUME, so while the session is behind or
 * yielded its client stops after one window. The input buffer is bounded
 * by the same number (SFTP_RX_MAX); past it is a client that ignored the
 * window, and the session ends.
 */

#include <axl.h>

#include "sshd-sftp-codec.h"
#include "sshd-sftp-iface.h"
#include "sshd-sftp.h"

AXL_LOG_DOMAIN("sftp");

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/// Open handles one session may hold (spec section 5).
#define SFTP_HANDLES_MAX     16u

/// Room for one reply: the largest is a DATA of SFTP_READ_MAX bytes; a
/// READDIR batch stops before it.
#define SFTP_OUT_CAP         (SFTP_READ_MAX + 1024u)

/// Most entries one READDIR reply carries (sftp-server's own batch).
#define SFTP_READDIR_BATCH   100u

/// Most volumes the virtual root lists.
#define SFTP_VOLUMES_MAX     64u

/// Longest `ls -l` line, NUL included.
#define SFTP_LONGNAME_MAX    (sizeof(((AxlFsEntry *)0)->name) + 80u)

/// Most bytes of a path a DEBUG line shows.
#define SFTP_LOG_PATH_MAX    128u

/// Exit statuses, as sftp-server's: a bad packet length, any other fatal.
#define SFTP_EXIT_BAD_LENGTH 11u
#define SFTP_EXIT_FATAL      255u

/// A file handle's position when it is not known (after an APPEND write or
/// a failed one): the next READ or WRITE seeks.
#define SFTP_POS_UNKNOWN     UINT64_MAX

/// The attributes SETSTAT and FSETSTAT act on; an EXTENDED part is ignored,
/// as sftp-server ignores it.
#define SFTP_ATTR_SETTABLE   (SFTP_ATTR_SIZE | SFTP_ATTR_UIDGID | SFTP_ATTR_PERMISSIONS \
                              | SFTP_ATTR_ACMODTIME)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// What a handle holds.
typedef enum {
    H_FREE = 0,
    H_FILE,       ///< an open file
    H_DIR,        ///< an open directory
    H_ROOT,       ///< the virtual root's listing
} HandleKind;

typedef struct {
    HandleKind  kind;
    uint32_t    gen;          ///< bumped each time the slot is reused
    AxlStream  *f;            ///< H_FILE
    uint64_t    pos;          ///< H_FILE: the stream's position, or SFTP_POS_UNKNOWN
    bool        can_read;     ///< H_FILE: opened for reading (READ, or no WRITE)
    bool        can_write;    ///< H_FILE: opened with WRITE
    bool        append;       ///< H_FILE: every WRITE goes to the end
    bool        wrote;        ///< H_FILE: a WRITE reached the stream: CLOSE flushes
    AxlDir     *d;            ///< H_DIR
    AxlVolume  *vols;         ///< H_ROOT: the volumes when it was opened
    size_t      nvols;        ///< H_ROOT
    size_t      next;         ///< H_ROOT: next volume to list
    bool        done;         ///< H_DIR / H_ROOT: listed to the end
    char        fs[SFTP_PATH_MAX];  ///< H_DIR: the AxlFs path (FSTAT). A file has none: it
                                    ///< is reached through its stream, which
                                    ///< follows a rename
} Handle;

struct SftpSession {
    SftpWriteFn    write;
    SftpEndFn      end;
    SftpConsumedFn consumed;
    void          *ctx;
    uint32_t    flags;
    size_t      write_max;    ///< largest write the channel accepts
    SftpRx      rx;
    uint8_t    *out;          ///< the reply being written (SFTP_OUT_CAP)
    size_t      out_len;      ///< its length
    size_t      out_off;      ///< bytes of it already written
    bool        init_done;    ///< INIT seen
    bool        eof;          ///< the client sent EOF
    bool        ended;        ///< end() called: nothing more is done
    bool        in_pump;      ///< pump() is on the stack
    bool        filled;       ///< a request zero-filled part of a file; not yet yielded for
    bool        yielded;      ///< waiting for the resume asked for: dispatch nothing
    Handle      h[SFTP_HANDLES_MAX];
};

/// One operation.
typedef void (*OpFn)(SftpSession *s, const SftpRequest *r, SftpOut *o);

/// A row of the dispatch table: sftp-server's `handlers[]`.
typedef struct {
    uint8_t          type;   ///< SFTP_* request type
    const char      *name;   ///< for the log, as sftp-server names it
    SftpRequestClass cls;    ///< read-only mode's class
    OpFn             fn;     ///< the operation
} OpRow;

/// A row of the extension table: sftp-server's `extended_handlers[]`.
typedef struct {
    const char      *name;   ///< the EXTENDED request name, e.g. "limits@openssh.com"
    const char      *data;   ///< what VERSION advertises with it (its version, "1")
    SftpRequestClass cls;    ///< SFTP_REQ_READS or SFTP_REQ_WRITES
    OpFn             fn;     ///< the operation
} ExtRow;

/// Where a normalised path lies, for the write operations' answers.
typedef enum {
    P_ROOT,       ///< `/`, the virtual root
    P_VOLUME,     ///< `/fs0`, a mapped volume's root
    P_TOP_NONE,   ///< `/x` naming no mapped volume: nothing, in `/`
    P_ENTRY,      ///< anything inside a volume
} PathKind;

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static void session_end(SftpSession *s, uint32_t status);
static bool flush(SftpSession *s);
static void pump(SftpSession *s);
static const OpRow *op_find(uint8_t type);
static const ExtRow *ext_find(SftpStr name);
static void op_extended(SftpSession *s, const SftpRequest *r, SftpOut *o);
static bool request_writes(const OpRow *op, const SftpRequest *r);
static void dispatch(SftpSession *s, const SftpRequest *r, SftpOut *o);
static Handle *handle_new(SftpSession *s, HandleKind kind, uint32_t *index);
static Handle *handle_get(SftpSession *s, SftpStr hs);
static void handle_free(Handle *h);
static void put_handle(SftpOut *o, uint32_t id, uint32_t index, const Handle *h);
static uint32_t path_of(const SftpStr *p, SftpPath *out);
static bool volume_mapped(const char *name);
static int  stat_path(const SftpPath *p, SftpAttrs *a);
static PathKind path_kind(const SftpPath *p);
static uint32_t parent_status(const SftpPath *p);
static bool path_exists(const SftpPath *p, PathKind kind);
static bool is_below(const SftpPath *a, const SftpPath *b);
static bool grow_ok(uint64_t size, uint64_t end);
static bool seek_to(Handle *h, uint64_t offset);
static uint32_t apply_attrs(SftpSession *s, const SftpAttrs *a, const char *fs,
                            AxlStream *f, uint32_t size_status, uint64_t old_size,
                            PathKind kind);
static uint32_t mtime_status(int rc);
static uint64_t now_unix(void);
static void op_init(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_unsupported(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_realpath(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_stat(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_fstat(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_open(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_read(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_write(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_opendir(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_readdir(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_close(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_setstat(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_fsetstat(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_remove(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_mkdir(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_rmdir(SftpSession *s, const SftpRequest *r, SftpOut *o);
static void op_rename(SftpSession *s, const SftpRequest *r, SftpOut *o);
static uint32_t rename_status(const SftpPath *from, const SftpPath *to);
static uint32_t move_source_status(const SftpPath *from, PathKind kf, const SftpPath *to);
static uint32_t rename_result(int rc);
static void op_posix_rename(SftpSession *s, const SftpRequest *r, SftpOut *o);
static uint32_t posix_rename_status(const SftpPath *from, const SftpPath *to);
static void op_limits(SftpSession *s, const SftpRequest *r, SftpOut *o);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static uint32_t     m_sessions;   ///< open sessions in this image
static SftpResumeFn m_resume;     ///< how a session that yields is resumed, or NULL

/**
 * Every request this server knows, with read-only mode's class: OpenSSH
 * sftp-server's `handlers[]` and its `does_write` column (SYMLINK writes
 * there, so it is refused here too before being answered unsupported). A
 * type missing from here is unknown -- logged, not answered -- and
 * AxlTestSshdSftp fails if sftp_decode() parses a type this table lacks.
 * EXTENDED is classed by its extension's name, from m_exts[].
 */
static const OpRow m_ops[] = {
    { SFTP_INIT,     "init",     SFTP_REQ_READS,  op_init },
    { SFTP_OPEN,     "open",     SFTP_REQ_OPEN,   op_open },
    { SFTP_CLOSE,    "close",    SFTP_REQ_READS,  op_close },
    { SFTP_READ,     "read",     SFTP_REQ_READS,  op_read },
    { SFTP_WRITE,    "write",    SFTP_REQ_WRITES, op_write },
    { SFTP_LSTAT,    "lstat",    SFTP_REQ_READS,  op_stat },
    { SFTP_FSTAT,    "fstat",    SFTP_REQ_READS,  op_fstat },
    { SFTP_SETSTAT,  "setstat",  SFTP_REQ_WRITES, op_setstat },
    { SFTP_FSETSTAT, "fsetstat", SFTP_REQ_WRITES, op_fsetstat },
    { SFTP_OPENDIR,  "opendir",  SFTP_REQ_READS,  op_opendir },
    { SFTP_READDIR,  "readdir",  SFTP_REQ_READS,  op_readdir },
    { SFTP_REMOVE,   "remove",   SFTP_REQ_WRITES, op_remove },
    { SFTP_MKDIR,    "mkdir",    SFTP_REQ_WRITES, op_mkdir },
    { SFTP_RMDIR,    "rmdir",    SFTP_REQ_WRITES, op_rmdir },
    { SFTP_REALPATH, "realpath", SFTP_REQ_READS,  op_realpath },
    { SFTP_STAT,     "stat",     SFTP_REQ_READS,  op_stat },
    { SFTP_RENAME,   "rename",   SFTP_REQ_WRITES, op_rename },
    { SFTP_READLINK, "readlink", SFTP_REQ_READS,  op_unsupported },
    { SFTP_SYMLINK,  "symlink",  SFTP_REQ_WRITES, op_unsupported },
    { SFTP_EXTENDED, "extended", SFTP_REQ_EXTENDED, op_extended },
};

/**
 * Every extension this server serves, with read-only mode's class:
 * sftp-server's `extended_handlers[]` and its `does_write` column. A name
 * missing from here is answered OP_UNSUPPORTED in every mode, as
 * sftp-server's process_extended() answers it. A served extension that
 * changes a volume (posix-rename@openssh.com) is SFTP_REQ_WRITES. NULL
 * ends the table.
 */
static const ExtRow m_exts[] = {
    { "posix-rename@openssh.com", "1", SFTP_REQ_WRITES, op_posix_rename },
    { "limits@openssh.com",       "1", SFTP_REQ_READS,  op_limits },
    { NULL, NULL, SFTP_REQ_UNKNOWN, NULL },
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void
sftp_set_resume(SftpResumeFn fn)
{
    m_resume = fn;
}

uint32_t
sftp_sessions_open(void)
{
    return m_sessions;
}

SftpRequestClass
sftp_request_class(uint8_t type)
{
    const OpRow *op = op_find(type);

    return op != NULL ? op->cls : SFTP_REQ_UNKNOWN;
}

SftpRequestClass
sftp_extension_class(const uint8_t *name, size_t len)
{
    if (name == NULL || len > UINT32_MAX) {
        return SFTP_REQ_UNKNOWN;
    }
    const ExtRow *ext = ext_find((SftpStr){ name, (uint32_t)len });

    return ext != NULL ? ext->cls : SFTP_REQ_UNKNOWN;
}

const char *
sftp_extension_name(size_t i)
{
    /* The last row is the NULL that ends the table. */
    return i < AXL_ARRAY_SIZE(m_exts) - 1u ? m_exts[i].name : NULL;
}

SftpSession *
sftp_session_open(SftpWriteFn write, SftpEndFn end, SftpConsumedFn consumed,
                  void *ctx, uint32_t flags, size_t write_max)
{
    if (write == NULL || end == NULL || consumed == NULL || write_max == 0) {
        return NULL;
    }
    SftpSession *s = axl_calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->out = axl_malloc(SFTP_OUT_CAP);
    if (s->out == NULL) {
        axl_free(s);
        return NULL;
    }
    s->write     = write;
    s->end       = end;
    s->consumed  = consumed;
    s->ctx       = ctx;
    s->flags     = flags;
    s->write_max = write_max;
    m_sessions++;
    axl_debug("session opened%s",
              (flags & SFTP_SESSION_READ_ONLY) != 0 ? " (read-only)" : "");
    return s;
}

void
sftp_session_close(SftpSession *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < SFTP_HANDLES_MAX; i++) {
        handle_free(&s->h[i]);
    }
    sftp_rx_free(&s->rx);
    axl_free(s->out);
    axl_free(s);
    m_sessions--;
    axl_debug("session closed");
}

void
sftp_session_data(SftpSession *s, const uint8_t *buf, size_t len)
{
    if (s == NULL || s->ended || len == 0) {
        return;
    }
    if (sftp_rx_feed(&s->rx, buf, len) != AXL_OK) {
        axl_warning("input buffer full (%u bytes held); ending the session",
                    (unsigned)(s->rx.len - s->rx.head));
        session_end(s, SFTP_EXIT_FATAL);
        return;
    }
    pump(s);
}

void
sftp_session_writable(SftpSession *s)
{
    /* The channel's own writable does NOT end a yield: it can fire in the
       same drain as the growth that yielded, and ending the yield there
       would run a second zero-fill before the loop served anyone else.
       Nothing is waiting to be written while yielded -- a session yields
       only once its reply has gone -- so it has nothing to do then. */
    if (s != NULL && !s->ended) {
        pump(s);
    }
}

void
sftp_session_resume(SftpSession *s)
{
    /* The resume the yield asked for: the loop has run, so requests may
       be dispatched again. The only call that ends a yield. */
    if (s != NULL && !s->ended) {
        s->yielded = false;
        pump(s);
    }
}

void
sftp_session_eof(SftpSession *s)
{
    if (s != NULL && !s->ended) {
        s->eof = true;
        pump(s);
    }
}

static void
session_end(SftpSession *s, uint32_t status)
{
    if (!s->ended) {
        s->ended = true;
        s->end(s->ctx, status);
    }
}

/* Write what is left of the current reply. False while the channel is
   behind (or the session has ended). */
static bool
flush(SftpSession *s)
{
    while (s->out_off < s->out_len) {
        size_t n = s->out_len - s->out_off;
        if (n > s->write_max) {
            n = s->write_max;
        }
        int rc = s->write(s->ctx, s->out + s->out_off, n);
        if (rc == AXL_BUSY) {
            return false;
        }
        if (rc != AXL_OK) {
            session_end(s, SFTP_EXIT_FATAL);
            return false;
        }
        s->out_off += n;
    }
    s->out_len = 0;
    s->out_off = 0;
    return true;
}

/* Answer every whole request held, one reply at a time, until the channel
   is behind. At the client's EOF with nothing left, the session ends, as
   sftp-server exits when its input ends.

   One zero-fill per dispatch: once a growth's reply has gone the session
   YIELDS -- asks to be resumed and dispatches nothing more, not what it
   holds and not what later data calls bring (AxlSsh delivers each
   CHANNEL_DATA in its own call, hundreds in one callback), until
   sftp_session_resume(). Data and EOF that arrive meanwhile are only
   buffered or marked, and the channel's writable changes nothing. */
static void
pump(SftpSession *s)
{
    if (s->in_pump || s->yielded) {
        return;
    }
    s->in_pump = true;
    while (!s->ended && flush(s)) {
        if (s->filled) {
            s->filled = false;
            if (m_resume != NULL && m_resume(s->ctx)) {
                axl_debug("yielding after a zero-fill until the loop resumes the session");
                s->yielded = true;
                break;
            }
        }
        const uint8_t *body = NULL;
        uint32_t       blen = 0;
        int            rc   = sftp_rx_next(&s->rx, &body, &blen);
        if (rc == AXL_INCOMPLETE) {
            if (s->eof) {
                session_end(s, 0);
            }
            break;
        }
        if (rc != AXL_OK) {
            axl_warning("bad packet length; ending the session");
            session_end(s, SFTP_EXIT_BAD_LENGTH);
            break;
        }
        /* Dispatched now: the client may send its packet's worth more. */
        s->consumed(s->ctx, 4u + (size_t)blen);
        SftpRequest r;
        if (sftp_decode(body, blen, &r) != AXL_OK) {
            axl_warning("malformed request (type %u); ending the session",
                        (unsigned)r.type);
            session_end(s, SFTP_EXIT_FATAL);
            break;
        }
        if (r.path.p != NULL) {
            char shown[SFTP_LOG_PATH_MAX];
            sftp_printable(shown, sizeof(shown), r.path.p, r.path.len);
            axl_debug("request %u type %u \"%s\"", (unsigned)r.id,
                      (unsigned)r.type, shown);
        } else {
            axl_debug("request %u type %u", (unsigned)r.id, (unsigned)r.type);
        }
        SftpOut o;
        sftp_out_init(&o, s->out, SFTP_OUT_CAP);
        dispatch(s, &r, &o);
        if (o.bad) {
            /* Every reply is sized to fit; reaching here is a defect. */
            axl_error("reply to type %u did not fit; ending the session",
                      (unsigned)r.type);
            session_end(s, SFTP_EXIT_FATAL);
            break;
        }
        s->out_len = o.len;
        s->out_off = 0;
    }
    s->in_pump = false;
}

static const OpRow *
op_find(uint8_t type)
{
    for (size_t i = 0; i < AXL_ARRAY_SIZE(m_ops); i++) {
        if (m_ops[i].type == type) {
            return &m_ops[i];
        }
    }
    return NULL;
}

static const ExtRow *
ext_find(SftpStr name)
{
    for (const ExtRow *e = m_exts; e->name != NULL; e++) {
        if (axl_strlen(e->name) == name.len
            && axl_memcmp(e->name, name.p, name.len) == 0) {
            return e;
        }
    }
    return NULL;
}

/* Whether @a r would change a volume: its row says so, it is an OPEN with
   a write flag, or it is an EXTENDED whose extension writes. An extension
   this server does not serve is not a write: it is answered OP_UNSUPPORTED
   in every mode. A row with no class counts as a write, so a table mistake
   refuses rather than writes. */
static bool
request_writes(const OpRow *op, const SftpRequest *r)
{
    const ExtRow *ext;

    switch (op->cls) {
    case SFTP_REQ_READS:
        return false;
    case SFTP_REQ_OPEN:
        return (r->pflags & SFTP_OPEN_WRITES) != 0;
    case SFTP_REQ_EXTENDED:
        ext = ext_find(r->ext);
        return ext != NULL && ext->cls != SFTP_REQ_READS;
    case SFTP_REQ_WRITES:
    case SFTP_REQ_UNKNOWN:
    default:
        return true;
    }
}

/* One request: found in the table, refused here when read-only mode forbids
   it -- before its operation runs, as sftp-server's request_permitted()
   refuses -- and otherwise handed to its operation. */
static void
dispatch(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    if (r->type != SFTP_INIT && !s->init_done) {
        axl_warning("request type %u before INIT; ending the session",
                    (unsigned)r->type);
        session_end(s, SFTP_EXIT_FATAL);
        return;
    }
    const OpRow *op = op_find(r->type);
    if (op == NULL) {
        /* sftp-server logs an unknown message and sends nothing. */
        axl_warning("unknown message %u", (unsigned)r->type);
        return;
    }
    if ((s->flags & SFTP_SESSION_READ_ONLY) != 0 && request_writes(op, r)) {
        axl_debug("refusing %s request in read-only mode", op->name);
        sftp_put_status(o, r->id, SFTP_FX_PERMISSION_DENIED, NULL);
        return;
    }
    op->fn(s, r, o);
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

static Handle *
handle_new(SftpSession *s, HandleKind kind, uint32_t *index)
{
    for (uint32_t i = 0; i < SFTP_HANDLES_MAX; i++) {
        Handle *h = &s->h[i];
        if (h->kind == H_FREE) {
            uint32_t gen = h->gen + 1u;
            axl_memset(h, 0, sizeof(*h));
            h->gen   = gen;
            h->kind  = kind;
            *index   = i;
            return h;
        }
    }
    return NULL;
}

static Handle *
handle_get(SftpSession *s, SftpStr hs)
{
    uint32_t index = 0;
    uint32_t gen   = 0;

    if (!sftp_handle_decode(hs, &index, &gen) || index >= SFTP_HANDLES_MAX) {
        return NULL;
    }
    Handle *h = &s->h[index];
    return h->kind != H_FREE && h->gen == gen ? h : NULL;
}

/* Release what @a h holds; its generation survives for the next use. */
static void
handle_free(Handle *h)
{
    axl_fclose(h->f);
    axl_dir_close(h->d);
    axl_free(h->vols);
    uint32_t gen = h->gen;
    axl_memset(h, 0, sizeof(*h));
    h->gen = gen;
}

static void
put_handle(SftpOut *o, uint32_t id, uint32_t index, const Handle *h)
{
    uint8_t b[SFTP_HANDLE_LEN];

    sftp_handle_encode(b, index, h->gen);
    sftp_put_handle(o, id, b, sizeof(b));
}

// ---------------------------------------------------------------------------
// Paths and attributes
// ---------------------------------------------------------------------------

/* Normalise a path from a request. SFTP_FX_OK, or the status to answer
   with: BAD_MESSAGE for one too long to be a path here, on the wire or once
   normalised (sftp-server's ENAMETOOLONG), NO_SUCH_FILE for any other it
   cannot name (a control byte: no FAT name has one). */
static uint32_t
path_of(const SftpStr *p, SftpPath *out)
{
    int rc = sftp_path_normalize(p->p, p->len, out);

    if (rc == AXL_OK) {
        return SFTP_FX_OK;
    }
    return rc == AXL_NO_RESOURCES ? SFTP_FX_BAD_MESSAGE : SFTP_FX_NO_SUCH_FILE;
}

static uint64_t
now_unix(void)
{
    AxlTimespec ts;

    return axl_clock_gettime(AXL_CLOCK_REALTIME, &ts) == AXL_OK && ts.tv_sec > 0
               ? (uint64_t)ts.tv_sec : 0;
}

/* Whether the firmware maps a volume called @a name (any case). */
static bool
volume_mapped(const char *name)
{
    AxlVolume vols[SFTP_VOLUMES_MAX];
    size_t    n = 0;

    if (axl_volume_enumerate(vols, SFTP_VOLUMES_MAX, &n) != AXL_OK) {
        return false;
    }
    for (size_t i = 0; i < n && i < SFTP_VOLUMES_MAX; i++) {
        if (axl_strcasecmp(vols[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Attributes of a normalised path: the virtual root, a volume, or a file. */
static int
stat_path(const SftpPath *p, SftpAttrs *a)
{
    AxlFsEntry e;

    if (p->fs[0] == '\0') {
        sftp_attrs_synth(a, true, false, 0, 0);
        return AXL_OK;
    }
    if (axl_file_info(p->fs, &e) == AXL_OK) {
        sftp_attrs_synth(a, axl_fs_entry_is_dir(&e), axl_fs_entry_is_read_only(&e),
                         e.size, e.mtime_unix);
        return AXL_OK;
    }
    /* A volume's root has no EFI_FILE_INFO name on every firmware: answer
       from the map when it is one. */
    if (axl_strchr(p->canon + 1, '/') == NULL && volume_mapped(p->volume)) {
        sftp_attrs_synth(a, true, false, 0, 0);
        return AXL_OK;
    }
    return AXL_NOT_FOUND;
}

static PathKind
path_kind(const SftpPath *p)
{
    if (p->fs[0] == '\0') {
        return P_ROOT;
    }
    if (axl_strchr(p->canon + 1, '/') != NULL) {
        return P_ENTRY;
    }
    return volume_mapped(p->volume) ? P_VOLUME : P_TOP_NONE;
}

/* Whether a new entry may be made at @a p, by its parent: SFTP_FX_OK; or
   PERMISSION_DENIED for a name in the virtual root (EACCES: the user does
   not own `/`); or NO_SUCH_FILE when the parent is missing or not a
   directory (ENOENT, ENOTDIR). @a p is P_TOP_NONE or P_ENTRY. */
static uint32_t
parent_status(const SftpPath *p)
{
    char      up[SFTP_PATH_MAX];
    SftpPath  parent;
    SftpAttrs a;

    if (path_kind(p) == P_TOP_NONE) {
        return SFTP_FX_PERMISSION_DENIED;
    }
    /* canon is "/vol/.../name": its parent is everything before the last
       '/', which is at least "/vol". */
    axl_strlcpy(up, p->canon, sizeof(up));
    char *slash = axl_strrstr(up, "/");
    if (slash == NULL || slash == up) {
        return SFTP_FX_NO_SUCH_FILE;
    }
    *slash = '\0';
    if (sftp_path_normalize((const uint8_t *)up, axl_strlen(up), &parent) != AXL_OK
        || stat_path(&parent, &a) != AXL_OK
        || (a.perm & SFTP_S_IFDIR) != SFTP_S_IFDIR) {
        return SFTP_FX_NO_SUCH_FILE;
    }
    return SFTP_FX_OK;
}

// ---------------------------------------------------------------------------
// Operations: reads
// ---------------------------------------------------------------------------

/* INIT: VERSION 3, advertising the served extensions in m_exts[] order. In
   read-only mode the writes are left out, as sftp-server -R leaves them out
   -- so sftp(1) falls back to SSH_FXP_RENAME, which is refused the same
   way. A second INIT is answered again, as sftp-server does. */
static void
op_init(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    const char *names[AXL_ARRAY_SIZE(m_exts)];
    const char *data[AXL_ARRAY_SIZE(m_exts)];
    size_t      n  = 0;
    bool        ro = (s->flags & SFTP_SESSION_READ_ONLY) != 0;

    (void)r;
    s->init_done = true;
    for (const ExtRow *e = m_exts; e->name != NULL; e++) {
        if (!ro || e->cls == SFTP_REQ_READS) {
            names[n] = e->name;
            data[n]  = e->data;
            n++;
        }
    }
    sftp_put_version(o, names, data, n);
}

/* limits@openssh.com: what this server accepts and sends. */
static void
op_limits(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    (void)s;
    sftp_put_limits(o, r->id, SFTP_PACKET_MAX, SFTP_READ_MAX, SFTP_WRITE_MAX,
                    SFTP_HANDLES_MAX);
}

/* EXTENDED: the extension's own operation, or OP_UNSUPPORTED for one this
   server does not serve ("Unknown extended request" in sftp-server). */
static void
op_extended(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    const ExtRow *ext = ext_find(r->ext);

    if (ext == NULL) {
        char shown[SFTP_LOG_PATH_MAX];
        sftp_printable(shown, sizeof(shown), r->ext.p, r->ext.len);
        axl_debug("unknown extended request \"%s\"", shown);
        sftp_put_status(o, r->id, SFTP_FX_OP_UNSUPPORTED, NULL);
        return;
    }
    ext->fn(s, r, o);
}

/* READLINK and SYMLINK: FAT has no links. */
static void
op_unsupported(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    (void)s;
    sftp_put_status(o, r->id, SFTP_FX_OP_UNSUPPORTED, NULL);
}

/* REALPATH: the canonical form, which must exist (as sftp-server's
   realpath(3) requires). */
static void
op_realpath(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath  p;
    SftpAttrs a;
    SftpAttrs none = { 0 };
    uint32_t  pc = path_of(&r->path, &p);

    (void)s;
    if (pc == SFTP_FX_OK && stat_path(&p, &a) != AXL_OK) {
        pc = SFTP_FX_NO_SUCH_FILE;
    }
    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    sftp_name_begin(o, r->id);
    sftp_name_add(o, p.canon, p.canon, &none);
    sftp_name_end(o, 1);
}

/* STAT and LSTAT: there are no links, so they are the same. */
static void
op_stat(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath  p;
    SftpAttrs a;
    uint32_t  pc = path_of(&r->path, &p);

    (void)s;
    if (pc == SFTP_FX_OK && stat_path(&p, &a) != AXL_OK) {
        pc = SFTP_FX_NO_SUCH_FILE;
    }
    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    sftp_put_attrs(o, r->id, &a);
}

static void
op_fstat(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle    *h = handle_get(s, r->handle);
    SftpAttrs  a;
    AxlFsEntry e;

    if (h == NULL) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    /* A file is asked through its own stream, so a rename since OPEN, or a
       new file at the old name, cannot redirect the answer. */
    int rc = h->kind == H_ROOT ? AXL_OK
             : h->kind == H_FILE ? axl_fstat(h->f, &e)
                                 : axl_file_info(h->fs, &e);
    if (h->kind == H_ROOT) {
        sftp_attrs_synth(&a, true, false, 0, 0);
    } else if (rc == AXL_OK) {
        sftp_attrs_synth(&a, axl_fs_entry_is_dir(&e), axl_fs_entry_is_read_only(&e),
                         e.size, e.mtime_unix);
    } else {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    sftp_put_attrs(o, r->id, &a);
}

/*
 * OPEN, for reading and for writing. pflags as sftp-server's
 * flags_from_portable(): WRITE opens for writing, READ (or no WRITE) for
 * reading, CREAT makes a missing file, EXCL then refuses an existing one,
 * TRUNC empties it, APPEND sends every WRITE to the end. Every check comes
 * before the handle is taken, and the handle before the volume is touched,
 * so a refusal -- the 17th handle included -- creates and truncates
 * nothing.
 */
static void
op_open(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath   p;
    AxlFsEntry e;
    uint32_t   index     = 0;
    bool       can_write = (r->pflags & SFTP_OPEN_WRITE) != 0;
    bool       creat     = (r->pflags & SFTP_OPEN_CREAT) != 0;
    bool       trunc     = (r->pflags & SFTP_OPEN_TRUNC) != 0;
    uint32_t   pc        = path_of(&r->path, &p);

    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    PathKind kind = path_kind(&p);
    if (kind == P_ROOT || kind == P_VOLUME) {
        /* A directory: EISDIR for a write, and the read rule for a read. */
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    bool exists = kind == P_ENTRY && axl_file_info(p.fs, &e) == AXL_OK;
    if (exists) {
        if (axl_fs_entry_is_dir(&e)
            || (creat && (r->pflags & SFTP_OPEN_EXCL) != 0)) {
            /* EISDIR, EEXIST */
            sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
            return;
        }
        if ((can_write || trunc) && axl_fs_entry_is_read_only(&e)) {
            /* EACCES: the attribute is the file's mode 0444. */
            sftp_put_status(o, r->id, SFTP_FX_PERMISSION_DENIED, NULL);
            return;
        }
    } else {
        pc = creat ? parent_status(&p) : SFTP_FX_NO_SUCH_FILE;
        if (pc != SFTP_FX_OK) {
            sftp_put_status(o, r->id, pc, NULL);
            return;
        }
    }
    Handle *h = handle_new(s, H_FILE, &index);
    if (h == NULL) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    /* "w" opens for reading and writing and creates a missing file; it
       never truncates. */
    h->f = axl_fopen(p.fs, can_write || !exists ? "w" : "r");
    if (h->f == NULL) {
        handle_free(h);
        sftp_put_status(o, r->id, can_write || !exists ? SFTP_FX_FAILURE
                                                       : SFTP_FX_PERMISSION_DENIED,
                        NULL);
        return;
    }
    if (exists && trunc && axl_file_truncate(p.fs, 0) != AXL_OK) {
        handle_free(h);
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    h->can_read  = (r->pflags & SFTP_OPEN_READ) != 0 || !can_write;
    h->can_write = can_write;
    h->append    = (r->pflags & SFTP_OPEN_APPEND) != 0;
    axl_debug("open \"%s\" flags 0x%x", p.canon, (unsigned)r->pflags);
    put_handle(o, r->id, index, h);
}

/* Whether a file of @a size bytes may be made to reach @a end: a shrink
   always, a growth by at most SFTP_GROW_MAX (see sshd-sftp.h for why). The
   caller marks the session `filled` once an allowed growth has actually
   happened -- a refused or failed one zero-filled nothing, and must not
   make pump() yield. */
static bool
grow_ok(uint64_t size, uint64_t end)
{
    if (end <= size || end - size <= SFTP_GROW_MAX) {
        return true;
    }
    axl_debug("refusing to grow a file by %llu bytes (at most %u)",
              (unsigned long long)(end - size), (unsigned)SFTP_GROW_MAX);
    return false;
}

/* Move @a h's stream to @a offset, unless it is there already. */
static bool
seek_to(Handle *h, uint64_t offset)
{
    if (h->pos == offset) {
        return true;
    }
    if (axl_fseek(h->f, (int64_t)offset, AXL_SEEK_SET) != AXL_OK) {
        h->pos = SFTP_POS_UNKNOWN;
        return false;
    }
    h->pos = offset;
    return true;
}

static void
op_read(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle *h = handle_get(s, r->handle);

    if (h == NULL || h->kind != H_FILE) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    size_t want = r->length > SFTP_READ_MAX ? SFTP_READ_MAX : r->length;
    if (r->offset > (uint64_t)INT64_MAX) {
        /* sftp-server's lseek() fails EINVAL, which it reports this way. */
        sftp_put_status(o, r->id, SFTP_FX_BAD_MESSAGE, NULL);
        return;
    }
    if (!h->can_read) {
        /* read(2) on a write-only descriptor: EBADF. */
        sftp_put_status(o, r->id, SFTP_FX_NO_SUCH_FILE, NULL);
        return;
    }
    if (!seek_to(h, r->offset)) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    uint8_t *dst = sftp_data_begin(o, r->id, want);
    if (dst == NULL) {
        return;   /* o->bad: pump ends the session */
    }
    size_t got = want > 0 ? axl_fread(dst, 1, want, h->f) : 0;
    if (got == 0 && want > 0) {
        /* Nothing read: rewrite the reply as EOF or a failure. Past the end
           the firmware's Read() fails (EFI_DEVICE_ERROR) where read(2)
           returns 0, so a failure there is still the end of the file. */
        bool       err = axl_ferror(h->f);
        AxlFsEntry e;
        if (err && axl_fstat(h->f, &e) == AXL_OK && r->offset >= e.size) {
            axl_clearerr(h->f);
            err = false;
        }
        sftp_out_init(o, o->p, o->cap);
        sftp_put_status(o, r->id, err ? SFTP_FX_FAILURE : SFTP_FX_EOF, NULL);
        return;
    }
    h->pos += got;
    sftp_data_end(o, got);
}

static void
op_opendir(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath   p;
    SftpAttrs  a;
    uint32_t   index = 0;
    uint32_t   pc    = path_of(&r->path, &p);

    if (pc == SFTP_FX_OK && stat_path(&p, &a) != AXL_OK) {
        pc = SFTP_FX_NO_SUCH_FILE;
    }
    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    if ((a.perm & SFTP_S_IFDIR) != SFTP_S_IFDIR) {
        /* sftp-server's ENOTDIR, which it reports as no such file. */
        sftp_put_status(o, r->id, SFTP_FX_NO_SUCH_FILE, NULL);
        return;
    }
    Handle *h = handle_new(s, p.fs[0] == '\0' ? H_ROOT : H_DIR, &index);
    if (h == NULL) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    if (h->kind == H_ROOT) {
        h->vols = axl_calloc(SFTP_VOLUMES_MAX, sizeof(AxlVolume));
        if (h->vols == NULL
            || axl_volume_enumerate(h->vols, SFTP_VOLUMES_MAX, &h->nvols) != AXL_OK) {
            handle_free(h);
            sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
            return;
        }
        if (h->nvols > SFTP_VOLUMES_MAX) {
            h->nvols = SFTP_VOLUMES_MAX;
        }
    } else {
        h->d = axl_dir_open(p.fs);
        if (h->d == NULL) {
            handle_free(h);
            sftp_put_status(o, r->id, SFTP_FX_PERMISSION_DENIED, NULL);
            return;
        }
        axl_strlcpy(h->fs, p.fs, sizeof(h->fs));
    }
    axl_debug("opendir \"%s\"", p.canon);
    put_handle(o, r->id, index, h);
}

static void
op_readdir(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle  *h = handle_get(s, r->handle);
    uint32_t count = 0;
    uint64_t now   = now_unix();
    char     ln[SFTP_LONGNAME_MAX];

    if (h == NULL || (h->kind != H_DIR && h->kind != H_ROOT)) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    sftp_name_begin(o, r->id);
    while (!h->done && count < SFTP_READDIR_BATCH) {
        /* Room for the largest entry still to come, or stop the batch. */
        if (o->cap - o->len < 2u * sizeof(ln) + 64u) {
            break;
        }
        SftpAttrs   a;
        const char *name = NULL;
        AxlFsEntry  e;
        if (h->kind == H_ROOT) {
            if (h->next >= h->nvols) {
                h->done = true;
                break;
            }
            name = h->vols[h->next++].name;
            sftp_attrs_synth(&a, true, false, 0, 0);
        } else {
            e.struct_size = sizeof(e);
            e.version     = AXL_FS_ENTRY_VERSION;
            if (!axl_dir_read(h->d, &e)) {
                h->done = true;
                break;
            }
            name = e.name;
            sftp_attrs_synth(&a, axl_fs_entry_is_dir(&e),
                             axl_fs_entry_is_read_only(&e), e.size, e.mtime_unix);
        }
        sftp_longname(ln, sizeof(ln), name, &a, now);
        sftp_name_add(o, name, ln, &a);
        count++;
    }
    if (count == 0) {
        sftp_out_init(o, o->p, o->cap);
        sftp_put_status(o, r->id, SFTP_FX_EOF, NULL);
        return;
    }
    sftp_name_end(o, count);
}

/* CLOSE. A file that was written is flushed first, so the status says
   whether the bytes reached the volume: closing a UEFI file cannot report
   a failure, and sftp-server's close(2) would. */
static void
op_close(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle *h = handle_get(s, r->handle);

    if (h == NULL) {
        /* sftp-server's handle_close() fails ENOENT for an unknown handle. */
        sftp_put_status(o, r->id, SFTP_FX_NO_SUCH_FILE, NULL);
        return;
    }
    bool ok = !h->wrote || axl_fflush(h->f) == AXL_OK;
    handle_free(h);
    sftp_put_status(o, r->id, ok ? SFTP_FX_OK : SFTP_FX_FAILURE, NULL);
}

// ---------------------------------------------------------------------------
// Operations: writes
// ---------------------------------------------------------------------------

/* WRITE: all of it at the offset (at the end under APPEND), or FAILURE. */
static void
op_write(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle *h = handle_get(s, r->handle);

    if (h == NULL || h->kind != H_FILE) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    if (!h->append && r->offset > (uint64_t)INT64_MAX) {
        /* lseek() EINVAL, as READ answers it. */
        sftp_put_status(o, r->id, SFTP_FX_BAD_MESSAGE, NULL);
        return;
    }
    if (!h->can_write) {
        /* write(2) on a read-only descriptor: EBADF. */
        sftp_put_status(o, r->id, SFTP_FX_NO_SUCH_FILE, NULL);
        return;
    }
    bool gap = false;
    if (!h->append && r->offset > 0) {
        /* A WRITE past the end zero-fills the gap: ask the file its size --
           the stream's own, not a remembered one, since another handle or a
           SETSTAT may have shrunk it and a READ past the end already moved
           the position. A GetInfo on an open file costs no I/O. */
        AxlFsEntry e;
        if (axl_fstat(h->f, &e) != AXL_OK || !grow_ok(e.size, r->offset)) {
            sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
            return;
        }
        gap = r->offset > e.size;
    }
    if (h->append) {
        h->pos = SFTP_POS_UNKNOWN;
        if (axl_fseek(h->f, 0, AXL_SEEK_END) != AXL_OK) {
            sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
            return;
        }
    } else if (!seek_to(h, r->offset)) {
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    size_t n = 0;
    if (r->data.len > 0) {
        h->wrote = true;
        n = axl_fwrite(r->data.p, 1, r->data.len, h->f);
    }
    /* A gap is zero-filled by the write that lands past it, so only a
       write that happened filled it (an empty WRITE moves nothing). */
    s->filled = s->filled || (gap && n > 0);
    if (n != r->data.len) {
        /* A short write leaves the position wherever the firmware left it. */
        h->pos = SFTP_POS_UNKNOWN;
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    if (!h->append) {
        h->pos += n;
    }
    sftp_put_status(o, r->id, SFTP_FX_OK, NULL);
}

/* An AxlFs result from setting a time, as sftp-server answers utimes(2):
   a time FAT cannot hold is EINVAL, a read-only file or handle EACCES. */
static uint32_t
mtime_status(int rc)
{
    switch (rc) {
    case AXL_OK:
        return SFTP_FX_OK;
    case AXL_INVALID:
        return SFTP_FX_BAD_MESSAGE;
    case AXL_DENIED:
        return SFTP_FX_PERMISSION_DENIED;
    case AXL_NOT_FOUND:
        return SFTP_FX_NO_SUCH_FILE;
    default:
        return SFTP_FX_FAILURE;
    }
}

/*
 * SETSTAT's and FSETSTAT's common part, in sftp-server's order: the size,
 * then permissions, times and owner, each failure replacing the status so
 * the last one is sent. The target is the handle's stream @a f when the
 * request names a handle (FSETSTAT), so it reaches the file the handle was
 * opened on wherever it now is, else the path @a fs, which lies where
 * @a kind says.
 *   - A size is applied. @a size_status is the status it gets before it
 *     is tried (SFTP_FX_OK to try it).
 *   - Times: the mtime is applied and the atime ignored -- FAT keeps no
 *     time of day for it. A volume root has no entry to hold a time, and
 *     answers OK with no change.
 *   - Permissions and owner are OK with no change, as sftp-server's chmod
 *     and chown succeed on a volume that keeps none.
 * On the virtual root, which the user does not own, all three are EPERM.
 */
static uint32_t
apply_attrs(SftpSession *s, const SftpAttrs *a, const char *fs, AxlStream *f,
            uint32_t size_status, uint64_t old_size, PathKind kind)
{
    uint32_t status = SFTP_FX_OK;
    bool     root   = kind == P_ROOT;

    if ((a->flags & SFTP_ATTR_SIZE) != 0) {
        if (size_status == SFTP_FX_OK
            && (f != NULL ? axl_ftruncate(f, a->size) : axl_file_truncate(fs, a->size))
                   != AXL_OK) {
            size_status = SFTP_FX_FAILURE;
        }
        if (size_status == SFTP_FX_OK && a->size > old_size) {
            /* The growth happened: it zero-filled, so pump() yields. */
            s->filled = true;
        }
        status = size_status;
    }
    if (root && (a->flags & SFTP_ATTR_PERMISSIONS) != 0) {
        status = SFTP_FX_PERMISSION_DENIED;
    }
    if ((a->flags & SFTP_ATTR_ACMODTIME) != 0) {
        uint32_t ts = SFTP_FX_OK;
        if (root) {
            ts = SFTP_FX_PERMISSION_DENIED;
        } else if (kind != P_VOLUME) {
            /* An earlier time is stored as the first one every volume keeps
               (1981-01-01), so `put -p` of an epoch-stamped file works where
               AxlFs refuses a pre-1980 time -- the clamp Linux vfat makes,
               moved a year on because the EDK2 FAT driver drops the year of
               any 1980 time. */
            uint64_t mt = a->mtime < AXL_FS_MTIME_KEPT_MIN ? AXL_FS_MTIME_KEPT_MIN
                                                           : a->mtime;
            ts = mtime_status(f != NULL ? axl_fset_mtime(f, mt) : axl_file_set_mtime(fs, mt));
        }
        if (ts != SFTP_FX_OK) {
            status = ts;
        }
    }
    if (root && (a->flags & SFTP_ATTR_UIDGID) != 0) {
        status = SFTP_FX_PERMISSION_DENIED;
    }
    return status;
}

static void
op_setstat(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath   p;
    AxlFsEntry e;
    uint32_t   pc = path_of(&r->path, &p);

    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    if ((r->attrs.flags & SFTP_ATTR_SETTABLE) == 0) {
        /* Nothing to set: sftp-server answers OK without looking. */
        sftp_put_status(o, r->id, SFTP_FX_OK, NULL);
        return;
    }
    PathKind kind = path_kind(&p);
    bool     dir  = kind == P_ROOT || kind == P_VOLUME;
    bool     ro   = false;
    if (!dir) {
        if (kind == P_TOP_NONE || axl_file_info(p.fs, &e) != AXL_OK) {
            /* ENOENT, whichever attribute meets it last. */
            sftp_put_status(o, r->id, SFTP_FX_NO_SUCH_FILE, NULL);
            return;
        }
        dir = axl_fs_entry_is_dir(&e);
        ro  = axl_fs_entry_is_read_only(&e);
    }
    /* truncate(2) on a directory is EISDIR; on a 0444 file, EACCES. */
    uint32_t size_status = dir ? SFTP_FX_FAILURE
                           : ro ? SFTP_FX_PERMISSION_DENIED
                                : SFTP_FX_OK;
    if (size_status == SFTP_FX_OK && (r->attrs.flags & SFTP_ATTR_SIZE) != 0
        && !grow_ok(e.size, r->attrs.size)) {
        size_status = SFTP_FX_FAILURE;
    }
    /* A size is applied only to an entry, whose e.size was read above. */
    sftp_put_status(o, r->id,
                    apply_attrs(s, &r->attrs, p.fs, NULL, size_status,
                                size_status == SFTP_FX_OK && !dir ? e.size : UINT64_MAX,
                                kind),
                    NULL);
}

static void
op_fsetstat(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    Handle *h = handle_get(s, r->handle);

    if (h == NULL || h->kind != H_FILE) {
        /* sftp-server's handle_to_fd() has no descriptor for a directory. */
        sftp_put_status(o, r->id, SFTP_FX_FAILURE, NULL);
        return;
    }
    /* ftruncate(2) on a descriptor not open for writing: EINVAL. */
    uint32_t size_status = h->can_write ? SFTP_FX_OK : SFTP_FX_BAD_MESSAGE;
    uint64_t old_size    = UINT64_MAX;
    if (size_status == SFTP_FX_OK && (r->attrs.flags & SFTP_ATTR_SIZE) != 0) {
        AxlFsEntry e;
        if (axl_fstat(h->f, &e) != AXL_OK || !grow_ok(e.size, r->attrs.size)) {
            size_status = SFTP_FX_FAILURE;
        } else {
            old_size = e.size;
        }
    }
    sftp_put_status(o, r->id,
                    apply_attrs(s, &r->attrs, NULL, h->f, size_status, old_size, P_ENTRY),
                    NULL);
}

/* REMOVE: a file. */
static void
op_remove(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath   p;
    AxlFsEntry e;
    uint32_t   pc = path_of(&r->path, &p);

    (void)s;
    if (pc == SFTP_FX_OK) {
        switch (path_kind(&p)) {
        case P_ROOT:
            pc = SFTP_FX_FAILURE;               /* EISDIR */
            break;
        case P_VOLUME:
            pc = SFTP_FX_PERMISSION_DENIED;     /* EACCES: `/` is not ours */
            break;
        case P_TOP_NONE:
            pc = SFTP_FX_NO_SUCH_FILE;
            break;
        case P_ENTRY:
        default:
            if (axl_file_info(p.fs, &e) != AXL_OK) {
                pc = SFTP_FX_NO_SUCH_FILE;
            } else if (axl_fs_entry_is_dir(&e)) {
                pc = SFTP_FX_FAILURE;           /* EISDIR */
            } else if (axl_fs_entry_is_read_only(&e)) {
                /* FAT's read-only attribute forbids deleting the file. */
                pc = SFTP_FX_PERMISSION_DENIED;
            } else {
                int rc = axl_file_delete(p.fs);
                pc = rc == AXL_OK          ? SFTP_FX_OK
                     : rc == AXL_NOT_FOUND ? SFTP_FX_NO_SUCH_FILE
                                           : SFTP_FX_FAILURE;
            }
            break;
        }
    }
    sftp_put_status(o, r->id, pc, NULL);
}

/* MKDIR. The attributes' permissions are ignored: FAT keeps none. */
static void
op_mkdir(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath p;
    uint32_t pc = path_of(&r->path, &p);

    (void)s;
    if (pc == SFTP_FX_OK) {
        if (path_exists(&p, path_kind(&p))) {
            pc = SFTP_FX_FAILURE;               /* EEXIST */
        } else {
            pc = parent_status(&p);
            if (pc == SFTP_FX_OK && axl_dir_mkdir(p.fs) != AXL_OK) {
                pc = SFTP_FX_FAILURE;
            }
        }
    }
    sftp_put_status(o, r->id, pc, NULL);
}

/* RMDIR: an empty directory. */
static void
op_rmdir(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath   p;
    AxlFsEntry e;
    uint32_t   pc = path_of(&r->path, &p);

    (void)s;
    if (pc == SFTP_FX_OK) {
        switch (path_kind(&p)) {
        case P_ROOT:
            pc = SFTP_FX_FAILURE;               /* EBUSY */
            break;
        case P_VOLUME:
            pc = SFTP_FX_PERMISSION_DENIED;     /* EACCES: `/` is not ours */
            break;
        case P_TOP_NONE:
            pc = SFTP_FX_NO_SUCH_FILE;
            break;
        case P_ENTRY:
        default:
            if (axl_file_info(p.fs, &e) != AXL_OK || !axl_fs_entry_is_dir(&e)) {
                pc = SFTP_FX_NO_SUCH_FILE;      /* ENOENT, ENOTDIR */
            } else if (axl_dir_rmdir(p.fs) != AXL_OK) {
                pc = SFTP_FX_FAILURE;           /* ENOTEMPTY and the rest */
            }
            break;
        }
    }
    sftp_put_status(o, r->id, pc, NULL);
}

/* Whether @a p names something: `/`, a volume, or an existing entry. */
static bool
path_exists(const SftpPath *p, PathKind kind)
{
    AxlFsEntry e;

    return kind == P_ROOT || kind == P_VOLUME
           || (kind == P_ENTRY && axl_file_info(p->fs, &e) == AXL_OK);
}

/* Whether @a b lies below @a a: "/fs0/d/x" below "/fs0/d", in any case, as
   FAT compares names. It compares the normalised text, so a spelling FAT
   folds to the same entry (an 8.3 alias, a trailing dot) is not seen: that
   move answers FAILURE rather than BAD_MESSAGE, and the driver still
   refuses it. */
static bool
is_below(const SftpPath *a, const SftpPath *b)
{
    size_t na = axl_strlen(a->canon);

    return axl_strncasecmp(a->canon, b->canon, na) == 0 && b->canon[na] == '/';
}

/*
 * RENAME: SSH_FXP_RENAME never replaces -- an existing target is FAILURE,
 * as sftp-server's link(2)-then-unlink answers it (posix-rename, which
 * replaces, is an extension). The checks follow sftp-server's: the source
 * must exist, the target must not, the target's directory must; then `/`
 * cannot move (EBUSY) nor a volume (EACCES), a read-only entry cannot move
 * (FAT's attribute), and a directory cannot move into itself (EINVAL).
 * Then axl_file_rename() moves it, within its directory or to another,
 * without copying; it refuses another volume (AXL_UNSUPPORTED, EXDEV).
 */
static void
op_rename(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpPath from;
    SftpPath to;
    uint32_t pc = path_of(&r->path, &from);

    (void)s;
    if (pc == SFTP_FX_OK) {
        pc = path_of(&r->path2, &to);
    }
    if (pc != SFTP_FX_OK) {
        sftp_put_status(o, r->id, pc, NULL);
        return;
    }
    sftp_put_status(o, r->id, rename_status(&from, &to), NULL);
}

/* op_rename()'s answer for two normalised paths, doing the rename when
   every check passes. */
static uint32_t
rename_status(const SftpPath *from, const SftpPath *to)
{
    PathKind kf = path_kind(from);

    if (!path_exists(from, kf)) {
        return SFTP_FX_NO_SUCH_FILE;
    }
    if (path_exists(to, path_kind(to))) {
        return SFTP_FX_FAILURE;                 /* EEXIST */
    }
    /* The target's directory: missing, or `/`. */
    uint32_t pc = parent_status(to);
    if (pc != SFTP_FX_OK) {
        return pc;
    }
    pc = move_source_status(from, kf, to);
    if (pc != SFTP_FX_OK) {
        return pc;
    }
    return rename_result(axl_file_rename(from->fs, to->fs));
}

/* What forbids moving @a from (of kind @a kf) to @a to whatever is there:
   `/` cannot move (EBUSY) nor a volume (EACCES), an entry with FAT's
   read-only attribute cannot, and a directory cannot move into itself
   (EINVAL). SFTP_FX_OK when nothing does. */
static uint32_t
move_source_status(const SftpPath *from, PathKind kf, const SftpPath *to)
{
    AxlFsEntry e;

    if (kf == P_ROOT) {
        return SFTP_FX_FAILURE;                 /* EBUSY */
    }
    if (kf == P_VOLUME) {
        return SFTP_FX_PERMISSION_DENIED;       /* EACCES */
    }
    if (axl_file_info(from->fs, &e) == AXL_OK && axl_fs_entry_is_read_only(&e)) {
        /* FAT's read-only attribute forbids renaming the entry. */
        return SFTP_FX_PERMISSION_DENIED;
    }
    if (is_below(from, to)) {
        return SFTP_FX_BAD_MESSAGE;             /* EINVAL: a directory into itself */
    }
    return SFTP_FX_OK;
}

/* An axl_file_rename() result as a status. One SetInfo moves the entry,
   within its directory or to another. An existing target is AXL_EXISTS
   (SSH_FXP_RENAME never replaces: EEXIST); another volume, or a filesystem
   that cannot move between directories, is AXL_UNSUPPORTED, answered as
   EXDEV is; anything else that fails is FAILURE. */
static uint32_t
rename_result(int rc)
{
    switch (rc) {
    case AXL_OK:
        return SFTP_FX_OK;
    case AXL_NOT_FOUND:
        return SFTP_FX_NO_SUCH_FILE;
    case AXL_EXISTS:
    case AXL_UNSUPPORTED:
    default:
        return SFTP_FX_FAILURE;
    }
}

/*
 * posix-rename@openssh.com: rename(2), which REPLACES an existing target --
 * sftp(1)'s `rename` uses it whenever the server advertises it. Statuses
 * are sftp-server's for rename(2)'s errno: a file onto a directory is
 * FAILURE (EISDIR), a directory onto a file NO_SUCH_FILE (ENOTDIR), onto a
 * directory that is not empty FAILURE (ENOTEMPTY); a directory onto an
 * empty one replaces it. A path naming the entry itself -- the same
 * spelling, or an 8.3 alias of it or of a directory on the way -- is OK and
 * changes nothing, as rename(2) of two links to one file; a change of letter
 * case alone renames it to that spelling.
 *
 * The arguments are two strings after the name; a request without them
 * ends the session, as sftp-server's parse of it is fatal.
 */
static void
op_posix_rename(SftpSession *s, const SftpRequest *r, SftpOut *o)
{
    SftpStr  a;
    SftpStr  b;
    SftpPath from;
    SftpPath to;

    if (sftp_ext_two_paths(r, &a, &b) != AXL_OK) {
        axl_warning("malformed posix-rename request; ending the session");
        session_end(s, SFTP_EXIT_FATAL);
        return;
    }
    uint32_t pc = path_of(&a, &from);
    if (pc == SFTP_FX_OK) {
        pc = path_of(&b, &to);
    }
    if (pc == SFTP_FX_OK) {
        axl_debug("posix-rename \"%s\" to \"%s\"", from.canon, to.canon);
        pc = posix_rename_status(&from, &to);
    }
    sftp_put_status(o, r->id, pc, NULL);
}

/* op_posix_rename()'s answer for two normalised paths. The virtual
   namespace is this server's own -- `/` and the volumes in it are not
   entries any filesystem holds -- so it is answered here, as for RENAME:
   `/` cannot move (EBUSY) nor a volume (EACCES), nothing can land on
   either (EBUSY), and a new name in `/` is not the user's (EACCES).
   Everything else -- identity (an 8.3 alias, a case-only change), the
   kind and emptiness rules, the read-only attribute, another volume, and
   the set-aside replace itself -- is axl_file_replace()'s, mapped to
   sftp-server's status for rename(2)'s errno. */
static uint32_t
posix_rename_status(const SftpPath *from, const SftpPath *to)
{
    PathKind kf = path_kind(from);
    PathKind kt = path_kind(to);

    if (kf == P_ROOT) {
        return SFTP_FX_FAILURE;                 /* EBUSY */
    }
    if (kf == P_VOLUME) {
        return SFTP_FX_PERMISSION_DENIED;       /* EACCES */
    }
    if (kf == P_TOP_NONE) {
        return SFTP_FX_NO_SUCH_FILE;            /* ENOENT */
    }
    if (kt == P_ROOT) {
        return SFTP_FX_FAILURE;                 /* onto `/`: EBUSY */
    }
    if (!path_exists(from, kf)) {
        /* rename(2) looks the old entry up before the new one's place. */
        return SFTP_FX_NO_SUCH_FILE;            /* ENOENT */
    }
    if (kt == P_VOLUME) {
        return SFTP_FX_FAILURE;                 /* onto a mount point: EBUSY */
    }
    if (kt == P_TOP_NONE) {
        return SFTP_FX_PERMISSION_DENIED;       /* a new name in `/`: EACCES */
    }
    switch (axl_file_replace(from->fs, to->fs, 0, NULL)) {
    case AXL_OK:
        return SFTP_FX_OK;
    case AXL_NOT_FOUND:
    case AXL_NOT_DIR:
        return SFTP_FX_NO_SUCH_FILE;            /* ENOENT, ENOTDIR */
    case AXL_INVALID:
        return SFTP_FX_BAD_MESSAGE;             /* EINVAL: a directory into itself */
    case AXL_DENIED:
        return SFTP_FX_PERMISSION_DENIED;       /* the read-only attribute */
    default:
        /* AXL_IS_DIR, AXL_NOT_EMPTY, AXL_UNSUPPORTED (EXDEV), AXL_EXISTS,
           AXL_NO_RESOURCES, AXL_CANCELLED, AXL_ERR. */
        return SFTP_FX_FAILURE;
    }
}
