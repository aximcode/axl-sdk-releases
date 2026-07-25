/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-internal.h
    Internal 9P2000.L codec + client state. Not a public header.
**/

#ifndef AXL_9P_INTERNAL_H
#define AXL_9P_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-inet-address.h>  /* AxlIPv4Address, needed by axl-tcp.h */
#include <axl/axl-tcp.h>           /* AxlTcp */

#define AXL_9P_NOFID      0xFFFFFFFFu
#define AXL_9P_NONUNAME   0xFFFFFFFFu
#define AXL_9P_MSIZE      8192u
#define AXL_9P_MIN_MSIZE  512u                    /* no real server negotiates below this */
#define AXL_9P_QID_LEN    13u
#define AXL_9P_MAX_XFER_BYTES   (256u * 1024u * 1024u)  /* read_file total cap */
#define AXL_9P_MAX_DIR_ENTRIES  (1u << 20)              /* list entry cap */

/* Rgetattr's st_mode S_IFDIR bit -- shared by axl-9p-mount.c (dir-vs-file on
   Tlopen) and axl-9p-client.c (reclassifying a dirent whose dtype lied). */
#define AXL_9P_S_IFDIR 0040000u

/* Linux EXDEV. The one definition of 18 in the tree: the SERVER answers it
   for a cross-directory Trename (s9p_handle_trename, via the P9_EXDEV alias
   in axl-9p-server-internal.h) and the CLIENT recognizes it to fall back to
   copy-then-unlink (axl_9p_rename). */
#define AXL_9P_EXDEV 18u

/* 9P2000.L message types. */
enum {
    AXL_9P_TVERSION = 100, AXL_9P_RVERSION = 101,
    AXL_9P_RLERROR  = 7,
    AXL_9P_TATTACH  = 104, AXL_9P_RATTACH  = 105,
    AXL_9P_TWALK    = 110, AXL_9P_RWALK    = 111,
    AXL_9P_TLOPEN   = 12,  AXL_9P_RLOPEN   = 13,
    AXL_9P_TREAD    = 116, AXL_9P_RREAD    = 117,
    AXL_9P_TREADDIR = 40,  AXL_9P_RREADDIR = 41,
    AXL_9P_TCLUNK   = 120, AXL_9P_RCLUNK   = 121,
    AXL_9P_TLCREATE = 14,  AXL_9P_RLCREATE = 15,
    AXL_9P_TRENAME  = 20,  AXL_9P_RRENAME  = 21,
    AXL_9P_TMKDIR   = 72,  AXL_9P_RMKDIR   = 73,
    AXL_9P_TWRITE   = 118, AXL_9P_RWRITE   = 119,
    AXL_9P_TREMOVE  = 122, AXL_9P_RREMOVE  = 123,
    AXL_9P_TGETATTR = 24,  AXL_9P_RGETATTR = 25,
    AXL_9P_TSETATTR = 26,  AXL_9P_RSETATTR = 27,
    AXL_9P_TFSYNC   = 50,  AXL_9P_RFSYNC   = 51,
};

/* Little-endian write cursor over a caller buffer. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;   ///< set true once a write would exceed cap
} Axl9pWriter;

/* Little-endian read cursor over a received buffer. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           error;   ///< set true once a read runs past len
} Axl9pReader;

void   axl_9p_w_init(Axl9pWriter *w, uint8_t *buf, size_t cap);
void   axl_9p_w_u8(Axl9pWriter *w, uint8_t v);
void   axl_9p_w_u16(Axl9pWriter *w, uint16_t v);
void   axl_9p_w_u32(Axl9pWriter *w, uint32_t v);
void   axl_9p_w_u64(Axl9pWriter *w, uint64_t v);
void   axl_9p_w_str(Axl9pWriter *w, const char *s);
void   axl_9p_w_bytes(Axl9pWriter *w, const void *data, size_t len);
/* Back-patch a little-endian u16 at byte offset `pos`, without moving the
   write cursor -- for a count field reserved before the items it counts
   (Twalk's nwname, Rwalk's nwqid), so an encoder can stream the items
   straight into the wire buffer instead of buffering them to learn the
   count first. Writes nothing and sets w->overflow if [pos, pos+2) isn't
   inside what has already been written. */
void   axl_9p_w_patch_u16(Axl9pWriter *w, size_t pos, uint16_t v);
/* Back-patch a little-endian u32 at byte offset `pos`, without moving the
   write cursor -- the u32 sibling of axl_9p_w_patch_u16, for the byte-count
   fields that precede a variable-length payload (Rread's count, Rreaddir's
   dcount). Writes nothing and sets w->overflow if [pos, pos+4) isn't inside
   what has already been written. */
void   axl_9p_w_patch_u32(Axl9pWriter *w, size_t pos, uint32_t v);
/* Reserve `n` bytes of payload in place and advance the cursor past them,
   returning a pointer to the reserved run so the caller can fill it
   DIRECTLY -- for a payload that is produced by something other than an
   axl_9p_w_* call (a file read straight into the reply buffer), where
   staging it in a bounce buffer first would mean a second copy and, at
   msize scale, a second 128 KiB allocation. Returns NULL and sets
   w->overflow if `n` doesn't fit; the reserved bytes are NOT zeroed, so a
   caller that fills fewer than `n` must patch the accompanying count field
   down (axl_9p_w_patch_u32) or treat the shortfall as an error. */
uint8_t *axl_9p_w_reserve(Axl9pWriter *w, size_t n);

void     axl_9p_r_init(Axl9pReader *r, const uint8_t *buf, size_t len);
uint8_t  axl_9p_r_u8(Axl9pReader *r);
uint16_t axl_9p_r_u16(Axl9pReader *r);
uint32_t axl_9p_r_u32(Axl9pReader *r);
uint64_t axl_9p_r_u64(Axl9pReader *r);
/* Copy a 9P string into out[cap] (NUL-terminated, truncated to cap-1).
   Returns the on-wire string length. */
size_t   axl_9p_r_str(Axl9pReader *r, char *out, size_t cap);

/* Begin a 9P message: reserves size[4] (patched by _finish) and writes
   type[1] tag[2]. Caller writes the body via axl_9p_w_* after this. */
void   axl_9p_msg_begin(Axl9pWriter *w, uint8_t *buf, size_t cap,
                        uint8_t type, uint16_t tag);
/* Patch the size[4] field with the final message length. Returns the
   total encoded length (also w->len). */
size_t axl_9p_msg_finish(Axl9pWriter *w);
/* Read the 7-byte 9P message header (size[4] type[1] tag[2]).
   Returns false if the reader underran. */
bool   axl_9p_msg_header(Axl9pReader *r, uint32_t *size,
                         uint8_t *type, uint16_t *tag);

/* Decide the msize to answer a Tversion with. Returns false when
   @a client_msize is below AXL_9P_MIN_MSIZE: such a session is unusable
   (every read/write chunk is msize minus a header, which would underflow)
   and the spec forbids answering with a LARGER msize than requested, so the
   only correct response is to refuse the session. On true, *out is
   min(client_msize, server_cap) and is always >= AXL_9P_MIN_MSIZE.
   @a server_cap must itself be >= AXL_9P_MIN_MSIZE. */
bool axl_9p_negotiate_msize(uint32_t client_msize, uint32_t server_cap,
                            uint32_t *out);

/* A connected 9P client session: one TCP socket + the negotiated msize +
   the fid allocator + the last reply buffer. */
struct Axl9pClient {
    AxlTcp  *sock;
    uint32_t msize;        ///< negotiated max message size
    uint32_t root_fid;     ///< fid attached to the tree root (0)
    uint32_t next_fid;     ///< monotonic fid allocator (root_fid + 1 ..)
    uint8_t  rbuf[AXL_9P_MSIZE];   ///< last reply
    size_t   rlen;         ///< length of the last reply
    uint32_t last_errno;   ///< errno from the most recent Rlerror; 0 if the
                           ///< last transact did not fail with one
    uint8_t  wbuf[AXL_9P_MSIZE];   ///< scratch for building a Twrite request
};

/* Send req[req_len], receive one reply into c->rbuf. On AXL_OK the reply
   type equals expect_type; an Rlerror or a type mismatch yields AXL_ERR.
   Takes `struct Axl9pClient *` (not the `Axl9pClient` typedef) so this
   header stays self-sufficient for TUs that include it without first
   pulling in the public <axl/axl-9p.h> typedef (e.g. axl-9p-codec.c). */
int axl_9p_transact(struct Axl9pClient *c, const uint8_t *req, size_t req_len,
                    uint8_t expect_type);

/* Client fid-level helpers, shared with the mount bridge (axl-9p-mount.c).
   All take `struct Axl9pClient *` for the same self-sufficiency reason as
   axl_9p_transact. Walk allocates a fresh fid pointing at @p path (caller
   clunks); lopen/lcreate open/create; getattr reads size/mtime/mode. */
int  axl_9p_client_walk(struct Axl9pClient *c, const char *path, uint32_t *out_fid);
int  axl_9p_client_lopen(struct Axl9pClient *c, uint32_t fid, uint32_t flags);
int  axl_9p_client_lcreate(struct Axl9pClient *c, uint32_t dfid, const char *name,
                           uint32_t flags, uint32_t mode);
void axl_9p_client_clunk(struct Axl9pClient *c, uint32_t fid);
int  axl_9p_client_split_parent(const char *path, char *parent, size_t cap,
                                const char **name_out);
int  axl_9p_client_getattr(struct Axl9pClient *c, uint32_t fid, uint64_t *out_size,
                           uint64_t *out_mtime, uint32_t *out_mode);

/* Opaque forward declaration -- keeps this header self-sufficient without
   pulling in <axl/axl-array.h> for TUs that only need the codec/client
   pieces (see the axl_9p_transact comment above for the same rationale). */
struct AxlArray;

/* Parse one Rreaddir reply body (the bytes AFTER the 7-byte msg header):
   dcount[4] then dcount bytes of entries (each qid[13] offset[8] type[1]
   name[s]). Appends every entry except "."/".." to `arr` as AxlFsEntry. On
   success returns AXL_OK, sets *out_dcount (0 => end of directory) and
   *out_last_off (the last entry's dirent-offset cursor, for the next
   Treaddir); *out_last_off is only meaningful when *out_dcount != 0.
   Returns AXL_ERR if the body is malformed (dcount claims more
   bytes than are present, or an entry runs past the end) -- callers must
   treat that as a hard error, NOT as EOF (a lying dcount must not be
   replayed forever against the same offset). */
int axl_9p_parse_readdir(const uint8_t *body, size_t body_len,
                         struct AxlArray *arr, uint32_t *out_dcount,
                         uint64_t *out_last_off);

#endif /* AXL_9P_INTERNAL_H */
