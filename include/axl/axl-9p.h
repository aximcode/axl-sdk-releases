/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p.h
    9P2000.L for UEFI — both ends of the protocol.

    The CLIENT connects to a 9P server over TCP to read and write files and
    directories, and can mount an export so ordinary AxlFs paths resolve
    through it. The SERVER exports an AxlFs subtree over TCP on an AxlLoop,
    read-write or read-only, to any 9P2000.L client — a Linux `mount -t 9p
    -o trans=tcp` included.

    Uses standard C types only; no EDK2 types leak.
**/

#ifndef AXL_9P_H
#define AXL_9P_H

#include <axl/axl-macros.h>
#include <axl/axl-bytes.h>
#include <axl/axl-array.h>
#include <axl/axl-fs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

/// An opaque connected 9P client session (one TCP connection + attached root).
typedef struct Axl9pClient Axl9pClient;

/**
 * @brief Connect to a 9P2000.L server over TCP and attach its root.
 *
 * Opens a TCP connection to @p host:@p port, negotiates protocol version
 * `9P2000.L` (fails if the peer will not), and attaches @p aname as the
 * session root.
 *
 * @return AXL_OK on success (@p out receives the session); AXL_ERR on a
 *     connection / negotiation / attach failure or NULL @p host / @p out.
 */
int
axl_9p_connect(
    const char   *host,    ///< server IPv4 string or hostname
    uint16_t      port,    ///< server port (9P default is 564)
    const char   *uname,   ///< user name; "" or NULL allowed
    const char   *aname,   ///< exported tree to attach; NULL/"" means "/"
    Axl9pClient **out      ///< [out] connected session
);

/**
 * @brief Close a 9P session and free it. NULL-safe.
 */
void
axl_9p_disconnect(
    Axl9pClient *c   ///< session from axl_9p_connect (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(Axl9pClient, axl_9p_disconnect)
#endif

/**
 * @brief Read a whole file from the server into a byte buffer.
 *
 * Walks to @p path, opens it read-only, and reads to EOF (chunked across
 * msize-bounded reads internally).
 *
 * @return AXL_OK on success (@p out receives an AxlBytes the caller frees
 *     with axl_bytes_unref); AXL_ERR on a missing path / read error / NULL arg.
 */
int
axl_9p_read_file(
    Axl9pClient *c,      ///< connected session
    const char  *path,   ///< absolute path on the server, '/'-separated
    AxlBytes   **out     ///< [out] file contents
);

/**
 * @brief Write a whole file to the server, creating or truncating it.
 *
 * Walks to @p path; if it exists, opens it write-only and truncates it; if it
 * does not, walks to its parent directory and creates it. Then writes @p len
 * bytes (chunked across msize-bounded writes internally) and closes it. The
 * parent directory must already exist (use @ref axl_9p_mkdir first otherwise).
 *
 * @return AXL_OK on success; AXL_ERR on a missing parent directory, a
 *     permission / write error, or NULL @p c / @p path (or NULL @p buf with
 *     non-zero @p len).
 */
int
axl_9p_write_file(
    Axl9pClient *c,      ///< connected session
    const char  *path,   ///< absolute path on the server, '/'-separated
    const void  *buf,    ///< bytes to write (may be NULL only when @p len is 0)
    size_t       len     ///< number of bytes
);

/**
 * @brief List a directory's entries.
 *
 * Walks to @p path, opens it, and reads all directory entries. Rreaddir's
 * wire dirent carries no size, so each entry the dirent's dtype did NOT
 * mark as a directory costs one extra Tgetattr round-trip to fill
 * `AxlFsEntry.size`; entries the dirent already marked as a directory are
 * left at 0 without a round-trip. Both halves are the same "file size in
 * bytes, 0 for directories" contract `axl_file_info` / `axl_dir_read`
 * publish. Because a dirent's dtype is not authoritative (a server may
 * report DT_UNKNOWN for a real directory), that Tgetattr round-trip also
 * reclassifies the entry from the reply's st_mode and re-zeros its size
 * when the mode says directory -- so the dtype-vs-mode disagreement never
 * reaches the caller as a wrong type with a wrong nonzero size.
 *
 * A per-entry stat that fails in isolation (a file unlinked between the
 * readdir and the walk) leaves just that entry's size at 0 and does not fail
 * the call. Three such failures in a row are read as a dead session: the call
 * stops early and returns AXL_ERR rather than hand back a listing whose sizes
 * are silently wrong. Only failures that reached the SERVER are counted --
 * an entry whose name makes the child path too long to build locally is
 * skipped the same way but says nothing about the session, so a run of them
 * cannot trip the dead-session verdict.
 *
 * @return AXL_OK on success (@p out receives an AxlArray of AxlFsEntry the
 *     caller frees with axl_array_free); AXL_ERR on a missing / non-directory
 *     path, a NULL arg, or a session that died during the per-entry stats.
 */
int
axl_9p_list(
    Axl9pClient *c,       ///< connected session
    const char  *path,    ///< absolute directory path on the server
    AxlArray   **out      ///< [out] AxlArray of AxlFsEntry
);

/**
 * @brief Create a directory on the server.
 *
 * Walks to @p path's parent (which must exist) and creates @p path's final
 * component as a directory (mode 0755). Not recursive.
 *
 * @return AXL_OK on success; AXL_ERR if the parent is missing, the name
 *     already exists, or on NULL args.
 */
int
axl_9p_mkdir(
    Axl9pClient *c,      ///< connected session
    const char  *path    ///< absolute directory path to create
);

/**
 * @brief Remove a file or empty directory from the server.
 *
 * Walks to @p path and removes it (the underlying `Tremove` also releases the
 * server-side handle). Directories must be empty.
 *
 * @return AXL_OK on success; AXL_ERR if @p path is missing, a non-empty
 *     directory, or on NULL args.
 */
int
axl_9p_remove(
    Axl9pClient *c,      ///< connected session
    const char  *path    ///< absolute path to remove
);

/**
 * @brief Rename / move a file or directory on the server.
 *
 * Walks to @p from and to @p to's parent directory (which must exist), then
 * moves @p from to @p to's final component within it. Both endpoints are on
 * the same server session.
 *
 * A SAME-DIRECTORY rename is a single server-side `Trename`.
 *
 * A CROSS-DIRECTORY rename is not, on every server that answers it
 * `Rlerror(EXDEV)` — which includes AXL's own Axl9pServer, deliberately, since
 * a server-side move would be an unbounded synchronous whole-file copy on its
 * event loop. Against such a server this call degrades to copy-then-unlink on
 * the CLIENT (read @p from, write @p to, remove @p from), the same fallback
 * mv(1) and every other rename(2) caller performs. Four consequences the
 * caller must plan for, because a copy is not a rename:
 *
 *   - A DIRECTORY is refused with AXL_ERR, not copied. A recursive tree copy
 *     has partial-failure semantics rename() does not have.
 *   - A file LARGER THAN 32 MiB is refused with AXL_ERR, not copied: the whole
 *     file is materialized in memory (peak ~2x the file size, since the read
 *     path accumulates into a string before copying into the returned
 *     buffer). Move it yourself in bounded steps.
 *   - If the copy succeeds but @p from cannot be removed, BOTH PATHS NOW
 *     EXIST. The destination is left in place and AXL_ERR is returned — the
 *     copy happened, the move did not. AXL_OK means both halves happened.
 *   - The copy does NOT preserve mode, mtime, or ownership: the write path
 *     creates @p to with mode 0644 and a fresh mtime under the attach user,
 *     where a real rename preserves all three. Inert against AXL's own
 *     FAT-backed server (which tracks none of them); NOT inert against a
 *     POSIX-backed one (diod, nfs-ganesha, kernel exportfs) — moving, say, a
 *     private key across directories this way strips its permissions.
 *
 * The fallback is not atomic and is not free: it transfers the file twice over
 * the session. A caller that cares should keep cross-directory moves off its
 * hot path. Unlike rename(2), it REFUSES an existing @p to rather than
 * overwriting it: rename(2)'s permission to clobber is only safe because the
 * replacement is atomic, and this fallback is not — a session drop between
 * the truncating open and the last write would leave a real destination
 * corrupted rather than either whole file. A caller that wants replace
 * semantics removes the destination first. This also matches the
 * same-directory path against Axl9pServer, whose `Trename` refuses a taken
 * destination outright.
 *
 * @return AXL_OK on success (for a cross-directory move, only if the copy AND
 *     the source removal both succeeded); AXL_ERR if @p from is missing,
 *     @p to's parent is missing, on NULL args, or if the EXDEV fallback was
 *     refused (directory / oversize / destination already exists) or failed
 *     part-way.
 */
int
axl_9p_rename(
    Axl9pClient *c,      ///< connected session
    const char  *from,   ///< existing absolute path
    const char  *to      ///< destination absolute path
);

/**
 * @brief Publish a live 9P connection as a UEFI fsN: volume.
 *
 * Bridges @p c onto an AxlFsProvider and calls axl_fs_provider_publish, so the
 * Shell and every UEFI app see a new fsN: backed by the remote 9P share. The
 * connection @p c must outlive the mount (it is borrowed, not owned) — call
 * axl_9p_unmount before axl_9p_disconnect.
 *
 * @return AXL_OK on success (@p out_volume receives an opaque token for
 *     axl_9p_unmount); AXL_ERR on NULL args or a publish failure.
 */
AXL_WARN_UNUSED int
axl_9p_mount(
    Axl9pClient *c,          ///< connected session (borrowed; must outlive the mount)
    bool         read_only,  ///< true = reject writes/creates/removes with EFI_WRITE_PROTECTED
    void       **out_volume  ///< [out] opaque token for axl_9p_unmount
);

/**
 * @brief Tear down a volume published by axl_9p_mount. NULL-safe.
 *
 * Force-closes any still-open handles (clunking their fids) and uninstalls the
 * filesystem protocols. The Axl9pClient is NOT disconnected — the caller still
 * owns it.
 *
 * @return AXL_OK on success; AXL_ERR if @p volume was not an axl_9p_mount token.
 */
int
axl_9p_unmount(
    void *volume   ///< token from axl_9p_mount (may be NULL)
);

// ============================ server =====================================
/// An opaque 9P2000.L server exporting an AxlFs subtree over TCP.
typedef struct Axl9pServer Axl9pServer;

/**
 * @brief Create a 9P server that exports @p root over TCP on @p loop.
 *
 * @p root is an AxlFs path prefix (e.g. "fs0:\\" or a RAM-disk volume) that
 * every client path resolves under. Read-write unless @p read_only. The server
 * runs on the caller's @p loop; pump the loop to service it. Does not listen
 * until axl_9p_server_listen.
 *
 * @return AXL_OK on success (@p out receives the server); AXL_ERR on NULL args,
 *     an EMPTY @p root (there is no useful export with no prefix, and a
 *     path that resolved against nothing would escape the tree), a @p root
 *     too long to store (256-byte internal limit), or allocation failure.
 */
AXL_WARN_UNUSED int
axl_9p_server_new(
    AxlLoop      *loop,       ///< event loop to run on
    const char   *root,       ///< AxlFs subtree to export (path prefix)
    bool          read_only,  ///< true = reject all mutating ops with EROFS
    Axl9pServer **out         ///< [out] new server
);

/**
 * @brief Begin accepting 9P clients on @p port.
 *
 * On failure the port is released before returning, so the caller may retry
 * (or hand @p port to another server) immediately.
 *
 * @return AXL_OK on success; AXL_ERR if already listening or the listen fails.
 */
AXL_WARN_UNUSED int
axl_9p_server_listen(
    Axl9pServer *s,     ///< server from axl_9p_server_new
    uint16_t     port   ///< TCP port; 0 selects the 9P default (564)
);

/**
 * @brief Stop the server and free it. Reaps all live connections. NULL-safe.
 *
 * The teardown is ABORTIVE and port-releasing, with no mode to choose:
 * freeing the server has no "keep serving" variant, and its callers go on to
 * free the `AxlLoop` it was built on. The listener and every live
 * connection are closed with a TCP RST and finalized **synchronously and
 * loop-free** — including the firmware accept backlog and any already-deferred
 * closes of connections accepted from this listener — so the listen port is
 * free on return and no close source is left registered on the loop. A fresh
 * @ref axl_9p_server_new + @ref axl_9p_server_listen on the same port
 * therefore succeeds immediately, with no loop pumping in between (EFI_TCP4
 * has no SO_REUSEADDR, so a deferred close would strand the port for the rest
 * of the boot). The trade is the documented one: the RST discards any un-ACKed
 * tail of an in-flight reply and clients see a reset rather than a clean FIN.
 * Connections dropped while the server is RUNNING are unaffected — those still
 * get a graceful FIN.
 */
void
axl_9p_server_free(
    Axl9pServer *s   ///< server (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(Axl9pServer, axl_9p_server_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_9P_H */
