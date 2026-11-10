/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-sftp-codec.h - SFTP version 3 (draft-ietf-secsh-filexfer-02) wire
 * codec and path normaliser, PURE: bytes in, bytes out, no file system and
 * no UEFI. The SFTP server (sshd-sftp.c) is the only user; the unit binary
 * AxlTestSshdSftp drives every function here exhaustively.
 *
 * Semantics follow OpenSSH's sftp-server.c, which every client is tested
 * against: a packet longer than SFTP_PACKET_MAX ends the session before
 * anything is allocated for it; a request that does not parse ends it too
 * (sftp-server's fatal on a short field); bytes left over at the end of a
 * request are ignored, as sftp-server discards them; status messages are
 * sftp-server's own texts.
 *
 * Every length read from the wire is checked against what is left of the
 * packet before it is used, so a decoded string always lies inside the
 * packet it came from.
 */

#ifndef AXL_TOOLS_SSHD_SFTP_CODEC_H
#define AXL_TOOLS_SSHD_SFTP_CODEC_H

#include <axl.h>

// ---------------------------------------------------------------------------
// Protocol constants (draft-ietf-secsh-filexfer-02)
// ---------------------------------------------------------------------------

/// The protocol version this server speaks (section 4).
#define SFTP_VERSION              3u

/// Largest packet accepted: its length field, excluding the field itself.
/// OpenSSH's SFTP_MAX_MSG_LENGTH.
#define SFTP_PACKET_MAX           (256u * 1024u)

/// Most READ data one reply carries -- OpenSSH's own cap. A READ asking for
/// more is answered with this much.
#define SFTP_READ_MAX             (32u * 1024u)

/// Most WRITE data one request may carry, as limits@openssh.com reports it:
/// a packet's worth less room for the request's own fields -- the figure
/// OpenSSH's sftp-server reports.
#define SFTP_WRITE_MAX            (SFTP_PACKET_MAX - 1024u)

/// Most bytes of input held while replies cannot leave: the most an
/// AXL_SSH_CHANNEL_WINDOW_ON_CONSUME channel can deliver unreported -- the
/// window and sshd's 10% grace -- since a request is reported consumed only
/// as it is dispatched. AxlSsh disconnects a client before it gets there,
/// so reaching it is a defect, and the session ends.
#define SFTP_RX_MAX               AXL_SSH_CHANNEL_UNCONSUMED_MAX

/// Longest path accepted from the wire, and the room the normaliser's
/// outputs have (NUL included).
#define SFTP_PATH_MAX             1024u

/// Longest volume name (`fs0`, a custom map name), NUL included.
#define SFTP_VOLUME_MAX           32u

/// Bytes in a handle string: a 4-byte slot index and a 4-byte generation.
#define SFTP_HANDLE_LEN           8u

/// Message types (section 3).
enum {
    SFTP_INIT           = 1,
    SFTP_VERSION_MSG    = 2,
    SFTP_OPEN           = 3,
    SFTP_CLOSE          = 4,
    SFTP_READ           = 5,
    SFTP_WRITE          = 6,
    SFTP_LSTAT          = 7,
    SFTP_FSTAT          = 8,
    SFTP_SETSTAT        = 9,
    SFTP_FSETSTAT       = 10,
    SFTP_OPENDIR        = 11,
    SFTP_READDIR        = 12,
    SFTP_REMOVE         = 13,
    SFTP_MKDIR          = 14,
    SFTP_RMDIR          = 15,
    SFTP_REALPATH       = 16,
    SFTP_STAT           = 17,
    SFTP_RENAME         = 18,
    SFTP_READLINK       = 19,
    SFTP_SYMLINK        = 20,
    SFTP_STATUS         = 101,
    SFTP_HANDLE         = 102,
    SFTP_DATA           = 103,
    SFTP_NAME           = 104,
    SFTP_ATTRS          = 105,
    SFTP_EXTENDED       = 200,
    SFTP_EXTENDED_REPLY = 201,
};

/// Status codes (section 7).
enum {
    SFTP_FX_OK                = 0,
    SFTP_FX_EOF               = 1,
    SFTP_FX_NO_SUCH_FILE      = 2,
    SFTP_FX_PERMISSION_DENIED = 3,
    SFTP_FX_FAILURE           = 4,
    SFTP_FX_BAD_MESSAGE       = 5,
    SFTP_FX_NO_CONNECTION     = 6,
    SFTP_FX_CONNECTION_LOST   = 7,
    SFTP_FX_OP_UNSUPPORTED    = 8,
};

/// Attribute flags (section 5).
#define SFTP_ATTR_SIZE         0x00000001u
#define SFTP_ATTR_UIDGID       0x00000002u
#define SFTP_ATTR_PERMISSIONS  0x00000004u
#define SFTP_ATTR_ACMODTIME    0x00000008u
#define SFTP_ATTR_EXTENDED     0x80000000u

/// OPEN pflags (section 6.3).
#define SFTP_OPEN_READ    0x00000001u
#define SFTP_OPEN_WRITE   0x00000002u
#define SFTP_OPEN_APPEND  0x00000004u
#define SFTP_OPEN_CREAT   0x00000008u
#define SFTP_OPEN_TRUNC   0x00000010u
#define SFTP_OPEN_EXCL    0x00000020u

/// The pflags that make an OPEN a write, for read-only mode: every flag but
/// READ. sftp-server -R admits READ with APPEND or EXCL, which cannot change
/// a byte by themselves; this server refuses any write flag, so the rule is
/// the flags alone.
#define SFTP_OPEN_WRITES  (SFTP_OPEN_WRITE | SFTP_OPEN_APPEND | SFTP_OPEN_CREAT \
                           | SFTP_OPEN_TRUNC | SFTP_OPEN_EXCL)

/// POSIX file-type and permission bits the attributes carry.
#define SFTP_S_IFDIR  0040000u
#define SFTP_S_IFREG  0100000u

/// Most `name=value` pairs an attribute block's EXTENDED part may carry.
#define SFTP_ATTR_EXT_MAX  64u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// A file's attributes (section 5). Only the fields @a flags names are
/// meaningful.
typedef struct {
    uint32_t flags;   ///< SFTP_ATTR_* present
    uint64_t size;    ///< SFTP_ATTR_SIZE
    uint32_t uid;     ///< SFTP_ATTR_UIDGID
    uint32_t gid;     ///< SFTP_ATTR_UIDGID
    uint32_t perm;    ///< SFTP_ATTR_PERMISSIONS: type and mode bits
    uint32_t atime;   ///< SFTP_ATTR_ACMODTIME, Unix seconds
    uint32_t mtime;   ///< SFTP_ATTR_ACMODTIME, Unix seconds
} SftpAttrs;

/// A borrowed byte string inside a decoded packet.
typedef struct {
    const uint8_t *p;    ///< first byte, inside the packet
    uint32_t       len;  ///< bytes
} SftpStr;

/**
 * One decoded request. Which fields are set depends on @a type; every
 * SftpStr points INTO the packet handed to sftp_decode(), so the packet must
 * outlive the request.
 *
 *  | type                          | fields                         |
 *  |-------------------------------|--------------------------------|
 *  | INIT                          | version                        |
 *  | OPEN                          | path, pflags, attrs            |
 *  | CLOSE, FSTAT, READDIR         | handle                         |
 *  | READ                          | handle, offset, length         |
 *  | WRITE                         | handle, offset, data           |
 *  | LSTAT, STAT, OPENDIR, REMOVE, RMDIR, REALPATH, READLINK | path  |
 *  | SETSTAT, MKDIR                | path, attrs                    |
 *  | FSETSTAT                      | handle, attrs                  |
 *  | RENAME                        | path (old), path2 (new)        |
 *  | SYMLINK                       | path (linkpath), path2 (target)|
 *  | EXTENDED                      | ext (name), data (the rest)    |
 *
 * Every type but INIT carries @a id.
 */
typedef struct {
    uint8_t   type;     ///< SFTP_* message type
    uint32_t  id;       ///< request id
    uint32_t  version;  ///< INIT: the client's version
    SftpStr   path;     ///< the (first) path
    SftpStr   path2;    ///< RENAME's new path, SYMLINK's target
    SftpStr   handle;   ///< the handle string
    uint64_t  offset;   ///< READ / WRITE
    uint32_t  length;   ///< READ: bytes asked for
    SftpStr   data;     ///< WRITE data; EXTENDED request-specific bytes
    SftpStr   ext;      ///< EXTENDED request name
    uint32_t  pflags;   ///< OPEN flags
    SftpAttrs attrs;    ///< OPEN / SETSTAT / FSETSTAT / MKDIR
} SftpRequest;

/**
 * Input framing: bytes from the channel, in arbitrary chunks, become whole
 * packets. Zero-initialise before use; sftp_rx_free() releases it.
 */
typedef struct {
    uint8_t *buf;   ///< held bytes (heap), or NULL
    size_t   len;   ///< bytes held
    size_t   cap;   ///< room in @a buf
    size_t   head;  ///< bytes at the front already handed out
} SftpRx;

/**
 * An output buffer: a reply is encoded into it. The caller provides the
 * storage. Writing past @a cap sets @a bad and writes nothing more, so an
 * encoder sequence is checked once, at the end.
 */
typedef struct {
    uint8_t *p;     ///< storage
    size_t   cap;   ///< its size
    size_t   len;   ///< bytes written
    size_t   start; ///< where the reply being written begins
    bool     bad;   ///< a write did not fit
} SftpOut;

/// A path in its two forms (see sftp_path_normalize()).
typedef struct {
    char canon[SFTP_PATH_MAX];    ///< "/fs0/a/b"; "/" for the root
    char fs[SFTP_PATH_MAX];       ///< "fs0:\\a\\b"; "fs0:\\" for a volume; "" for the root
    char volume[SFTP_VOLUME_MAX]; ///< "fs0", lower-case; "" for the root
} SftpPath;

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/**
 * Append @p len bytes from the channel.
 *
 * @return AXL_OK; AXL_NO_RESOURCES when they cannot be held, or when the
 *     bytes held would pass SFTP_RX_MAX: the session must end.
 */
AXL_WARN_UNUSED int
sftp_rx_feed(
    SftpRx        *rx,   ///< framing state
    const uint8_t *buf,  ///< bytes from the channel
    size_t         len   ///< how many
);

/**
 * The next whole packet, if one is held: its body (type byte onwards),
 * without the length field. The body stays valid until the next
 * sftp_rx_feed(), sftp_rx_next() or sftp_rx_free().
 *
 * The length is checked the moment its four bytes are held, before the
 * rest of the packet arrives: a length of 0 or over SFTP_PACKET_MAX is
 * AXL_INVALID at once.
 *
 * @return AXL_OK with @p body and @p len set; AXL_INCOMPLETE when no whole packet
 *     is held yet; AXL_INVALID for a bad length (the session must end).
 */
AXL_WARN_UNUSED int
sftp_rx_next(
    SftpRx         *rx,    ///< framing state
    const uint8_t **body,  ///< [out] the packet body
    uint32_t       *len    ///< [out] its length
);

/// Release what @p rx holds and zero it. NULL-safe.
void
sftp_rx_free(
    SftpRx *rx   ///< framing state
);

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/**
 * Decode one packet body (from sftp_rx_next()).
 *
 * A type this server does not know still decodes, with only @a type and
 * @a id set, so the caller can answer it as sftp-server does (no reply for
 * an unknown type). A path is bounded only by the packet: one too long for
 * the server (sftp_path_normalize() refuses it) is the session's to answer
 * with a status, as sftp-server answers ENAMETOOLONG, not a reason to end
 * the session. A handle string longer than 256 bytes does not parse.
 * Trailing bytes after the fields a type defines are ignored.
 *
 * @return AXL_OK; AXL_INVALID when a field is missing or a length runs past
 *     the packet (the session must end, as in sftp-server); @p r is then
 *     undefined except @a type, and @a id when it was read.
 */
AXL_WARN_UNUSED int
sftp_decode(
    const uint8_t *body,  ///< packet body: type byte first
    size_t         len,   ///< its length
    SftpRequest   *r      ///< [out] the request
);

/**
 * The two path strings an EXTENDED request carries after its name -- the
 * arguments of posix-rename@openssh.com (old path, then new) -- from
 * @p r->data. Bytes after the second string are ignored, as sftp-server
 * ignores them.
 *
 * @return AXL_OK; AXL_INVALID when @p r is not an EXTENDED request or the
 *     data does not hold two whole strings: sftp-server ends the session
 *     on that, and so does the caller.
 */
AXL_WARN_UNUSED int
sftp_ext_two_paths(
    const SftpRequest *r,     ///< a decoded EXTENDED request
    SftpStr           *first, ///< [out] the first string
    SftpStr           *second ///< [out] the second string
);

/**
 * Decode an attribute block at @p *off, advancing it.
 *
 * @return AXL_OK; AXL_INVALID when it runs past @p len or carries more than
 *     SFTP_ATTR_EXT_MAX extended pairs.
 */
AXL_WARN_UNUSED int
sftp_attrs_decode(
    const uint8_t *p,     ///< bytes
    size_t         len,   ///< their length
    size_t        *off,   ///< [in,out] read position
    SftpAttrs     *a      ///< [out] attributes
);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/// Start writing replies into @p storage.
void
sftp_out_init(
    SftpOut *o,        ///< output buffer
    void    *storage,  ///< where the bytes go
    size_t   cap       ///< its size
);

/// sftp-server's text for a status code: "Success", "End of file", ...,
/// "Unknown error" for any code it does not name.
const char *
sftp_status_message(
    uint32_t code   ///< SFTP_FX_*
);

/// VERSION (3) advertising the extensions in @p ext_names / @p ext_data:
/// @p n `name`/`data` pairs, both NUL-terminated strings.
void
sftp_put_version(
    SftpOut           *o,          ///< output
    const char *const *ext_names,  ///< extension names, or NULL when @p n is 0
    const char *const *ext_data,   ///< their data strings
    size_t             n           ///< pairs
);

/// STATUS: @p code with @p msg, or sftp_status_message(@p code) when
/// @p msg is NULL, and an empty language tag, as sftp-server sends.
void
sftp_put_status(
    SftpOut    *o,     ///< output
    uint32_t    id,    ///< request id
    uint32_t    code,  ///< SFTP_FX_*
    const char *msg    ///< message, or NULL for the code's own
);

/// EXTENDED_REPLY to limits@openssh.com: max-packet-length,
/// max-read-length, max-write-length and max-open-handles, each a uint64,
/// in that order (OpenSSH's PROTOCOL file).
void
sftp_put_limits(
    SftpOut  *o,        ///< output
    uint32_t  id,       ///< request id
    uint64_t  packet,   ///< largest packet accepted
    uint64_t  read,     ///< most data one READ reply carries
    uint64_t  write,    ///< most data one WRITE may carry
    uint64_t  handles   ///< most handles open at once
);

/// HANDLE: the @p len handle bytes.
void
sftp_put_handle(
    SftpOut       *o,    ///< output
    uint32_t       id,   ///< request id
    const uint8_t *h,    ///< handle bytes
    size_t         len   ///< their count
);

/**
 * Start a DATA reply of up to @p max bytes and return where they go, so a
 * read lands straight in the reply; sftp_data_end() then records how many
 * were written. NULL (and @a bad set) when @p max does not fit.
 */
uint8_t *
sftp_data_begin(
    SftpOut  *o,    ///< output
    uint32_t  id,   ///< request id
    size_t    max   ///< most bytes the read may write
);

/// Finish the DATA reply sftp_data_begin() started, carrying @p n bytes
/// (n <= its max).
void
sftp_data_end(
    SftpOut *o,   ///< output
    size_t   n    ///< bytes written at the pointer begin returned
);

/// ATTRS: @p a.
void
sftp_put_attrs(
    SftpOut         *o,   ///< output
    uint32_t         id,  ///< request id
    const SftpAttrs *a    ///< attributes
);

/// Encode @p a alone (no packet header): the block NAME entries carry.
void
sftp_attrs_encode(
    SftpOut         *o,   ///< output
    const SftpAttrs *a    ///< attributes
);

/// Start a NAME reply; entries follow with sftp_name_add(), and
/// sftp_name_end() writes the count.
void
sftp_name_begin(
    SftpOut  *o,   ///< output
    uint32_t  id   ///< request id
);

/// One NAME entry: @p name, its `ls -l` @p longname and @p a.
void
sftp_name_add(
    SftpOut         *o,         ///< output
    const char      *name,      ///< file name
    const char      *longname,  ///< `ls -l` line
    const SftpAttrs *a          ///< attributes
);

/// Finish the NAME reply sftp_name_begin() started, with @p count entries.
void
sftp_name_end(
    SftpOut  *o,      ///< output
    uint32_t  count   ///< entries added
);

/// The bytes the reply started last needs: its packet so far. A NAME reply
/// being built is measured by this to stop before it passes a budget.
size_t
sftp_out_reply_len(
    const SftpOut *o   ///< output
);

// ---------------------------------------------------------------------------
// Attributes and listings
// ---------------------------------------------------------------------------

/**
 * Attributes for a file system entry: size, type and permissions (0644 for
 * a file, 0755 for a directory, with the write bits cleared when
 * @p read_only), and atime = mtime = @p mtime_unix (clamped to 32 bits).
 */
void
sftp_attrs_synth(
    SftpAttrs *a,           ///< [out] attributes
    bool       is_dir,      ///< a directory
    bool       read_only,   ///< no write bits
    uint64_t   size,        ///< bytes (files)
    uint64_t   mtime_unix   ///< modification time, Unix seconds
);

/**
 * The `ls -l` line sftp-server writes for a NAME entry (OpenSSH's
 * ls_file()): mode, link count 1, owner and group 0, size, date and name,
 * e.g. `-rw-r--r--    1 0        0            1234 Jan  2 03:04 name`. The
 * date shows the time of day when it lies within the half year before
 * @p now_unix and the year otherwise, as ls(1) does; times are UTC.
 */
void
sftp_longname(
    char            *out,       ///< [out] the line, NUL-terminated
    size_t           cap,       ///< its room (truncated to fit)
    const char      *name,      ///< file name
    const SftpAttrs *a,         ///< its attributes
    uint64_t         now_unix   ///< the time now, Unix seconds
);

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

/// Encode slot @p index and @p gen as a SFTP_HANDLE_LEN-byte handle.
void
sftp_handle_encode(
    uint8_t  out[SFTP_HANDLE_LEN],  ///< [out] handle bytes
    uint32_t index,                  ///< slot
    uint32_t gen                     ///< the slot's generation
);

/// Decode a handle string. False when it is not SFTP_HANDLE_LEN bytes.
bool
sftp_handle_decode(
    SftpStr   h,       ///< handle string from a request
    uint32_t *index,   ///< [out] slot
    uint32_t *gen      ///< [out] generation
);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

/**
 * Normalise a path from the wire.
 *
 * Accepted, all at once:
 *   - `/fs0/dir/file`, the canonical form;
 *   - `fs0:/dir/file`, `FS0:\dir\file`, `fs0:dir` -- the Shell form (a
 *     first component ending in `:`);
 *   - a relative path, resolved against `/` (SFTP has no server-side
 *     working directory: clients resolve against REALPATH(".")).
 * `/` and `\` are both separators. Empty components and `.` are dropped;
 * `..` removes the previous component and never climbs above `/`. The
 * volume (the first component) is lower-cased; every other component is
 * kept as sent.
 *
 * @p out->canon is the canonical form, `/` for the root and `/fs0` for a
 * volume. @p out->fs is what AxlFs takes -- `fs0:\dir\file`, `fs0:\` for a
 * volume -- and empty for the root, which is the server's own virtual
 * directory. @p out->volume is the volume's name, empty for the root.
 *
 * @return AXL_OK; AXL_NO_RESOURCES when the path, or either form of the
 *     result, does not fit SFTP_PATH_MAX -- the Shell form grows by one
 *     byte as `fs0:` becomes `/fs0/`, so a 1023-byte input can be refused
 *     here; AXL_INVALID for a NUL or control byte (below 0x20, or 0x7f)
 *     anywhere, an empty volume name before `:`, or a volume name longer
 *     than SFTP_VOLUME_MAX - 1 bytes. @p out is undefined on failure.
 */
AXL_WARN_UNUSED int
sftp_path_normalize(
    const uint8_t *in,    ///< path bytes (not NUL-terminated)
    size_t         len,   ///< their count
    SftpPath      *out    ///< [out] both forms
);

#endif /* AXL_TOOLS_SSHD_SFTP_CODEC_H */
