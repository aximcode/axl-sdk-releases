# Axl9p Phase 4 — Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A native async 9P2000.L **server** (`Axl9pServer`) that exports an `AxlFs` subtree over TCP, so a Linux/WSL host can `mount -t 9p -otrans=tcp` a UEFI box's storage — the differentiated capability HTTP/WebDAV can't provide.

**Architecture:** `Axl9pServer` runs on the caller's `AxlLoop` using the async `AxlTcp` API (`axl_tcp_listen` + `axl_tcp_accept_async` + `axl_tcp_recv_async` + `axl_tcp_send_async`), mirroring `src/net/axl-http-server.c`'s accept/per-connection model. Each connection owns a fid table (fid → `{ AxlFs path, open dir/file state }`) and a receive buffer that reassembles size-prefixed 9P messages across partial recvs; a dispatcher decodes each T-message and emits the matching R-message (or `Rlerror(errno)`) built on the existing `src/9p/axl-9p-codec.c` writers. The backend is `AxlFs` path ops (`axl_file_get_bytes`/`axl_file_writer_*`/`axl_dir_open`/`_read`/`axl_file_info`/`axl_dir_mkdir`/`axl_file_delete`/`axl_file_rename`). `--ro` answers every mutating op with `Rlerror(EROFS)`.

**Tech Stack:** C (AXL public types only in `include/axl/axl-9p.h`: `Axl9pServer`, `AxlLoop`, standard C types), internal `src/9p/axl-9p-internal.h` codec + a new `src/9p/axl-9p-server.c`, `AxlFs` backend, QEMU integration via a host Python 9P client (`p9-client.py`) reaching the guest server through `test_add_port_forward`.

## Global Constraints

- Standard C types only in the public header; no EDK2 types leak. (spec §6)
- 9P2000.L only; little-endian wire; header `size[4] type[1] tag[2]`; `Rlerror(type=7)` carries a Linux errno. (spec §7)
- `msize` negotiated in `Rversion` (default 8192, floor `AXL_9P_MIN_MSIZE`=512); reads/writes chunk to `msize - header`; the server MUST bound every incoming count/offset/length against the buffer it will read into or write from — a hostile client must never overrun `conn` buffers. (spec §7, §9)
- One request in flight at a time per connection (v1); server handles one tag at a time per connection. (spec §7)
- `--ro` server: `Twrite`/`Tlcreate`/`Tmkdir`/`Tremove`/`Trename`/`Tsetattr` → `Rlerror(EROFS=30)`. (spec §9)
- `Tversion` mismatch (peer won't do `9P2000.L`) → the server replies `Rversion` with `version="9P2000.L"` only if it can; a client that then insists on another version gets errors — no downgrade. (spec §4, §7)
- A connection drop / malformed message → reap the connection (free its fid table + buffers), like the HTTP server's `reset_connection`. Never fault on hostile input. (spec §9)
- Test-first, both arches (X64 + `--arch AARCH64`); exact-string assertions.
- Errno values (Linux): `EPERM=1 ENOENT=2 EIO=5 EBADF=9 EACCES=13 EEXIST=17 ENOTDIR=20 EISDIR=21 EINVAL=22 ENOSPC=28 EROFS=30 ENOTEMPTY=39 EPROTO=71`. Map `AxlFsStatus`/`AxlStatus` failures onto these for `Rlerror`.

## File Structure

- `include/axl/axl-9p.h` — Modify: add the `Axl9pServer` opaque type + `axl_9p_server_new` / `_listen` / `_free` decls + docs.
- `src/9p/axl-9p-internal.h` — Modify: add any server-only message-type constants not already present (`Tsetattr`=26/`Rsetattr`=27, `Tfsync`=50/`Rfsync`=51, `Treadlink`? NO — out of scope), and declare the server-side codec helpers (`axl_9p_parse_header_body`, an `Axl9pReader`-based body parse) if factored out. Most codec is reused.
- `src/9p/axl-9p-server.c` — Create: the whole server — listen/accept, per-connection struct + fid table + recv framing, the dispatcher, and all message handlers. This is the bulk. If it grows past ~900 lines, split handlers into `src/9p/axl-9p-server-ops.c` (report as DONE_WITH_CONCERNS and split only with controller guidance).
- `test/integration/p9-client.py` — Create: a host-side 9P2000.L client (connect/version/attach/walk/lopen/read/readdir/write/getattr/mkdir/remove/rename) used to drive the guest server and assert.
- `test/integration/9p-server-selftest.c` — Create: the guest UEFI app — seed a known AxlFs tree (a RAM disk with `hello.txt` + a subdir), start `Axl9pServer` on a loop, listen on a port, pump the loop until a deadline.
- `test/integration/test-9p-server-qemu.sh` — Create: boot the guest server app with a host↔guest port forward, run `p9-client.py` against `127.0.0.1:HOST_PORT`, assert ls/read/write/mkdir/remove byte-exact.
- `Makefile` — Modify: add `src/9p/axl-9p-server.c` to the library sources + a `9p-server-selftest` target.
- `src/9p/README.md`, `docs/sphinx/modules/9p.rst` — Modify: document the server.

---

## Task 1: Server-only codec message types + version/attach reply builders

Add the message types the server needs beyond the client set, and unit-test encode/decode of the server-direction replies that have non-trivial bodies (`Rversion`, `Rattach`, `Rwalk`, `Rread`, `Rreaddir`, `Rlerror`).

**Files:**
- Modify: `src/9p/axl-9p-internal.h`
- Test: `test/unit/axl-test-9p.c`

**Interfaces:**
- Consumes: existing codec (`axl_9p_msg_begin`/`_finish`, `axl_9p_w_*`/`axl_9p_r_*`, `axl_9p_msg_header`, `AXL_9P_R*` constants).
- Produces: `AXL_9P_TSETATTR=26`, `AXL_9P_RSETATTR=27`, `AXL_9P_TFSYNC=50`, `AXL_9P_RFSYNC=51` (constants for later tasks).

- [ ] **Step 1: Write the failing test** — append to `test/unit/axl-test-9p.c` a `test_9p_server_reply_codec` that builds an `Rversion`, `Rwalk` (nwqid + qids), and `Rlerror`, and decodes each back, asserting exact fields (msize round-trips; nwqid count; errno). Use the existing `Axl9pWriter`/`Reader` helpers. Register it in the runner.

```c
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
    for (int q = 0; q < 2; q++) { for (int b = 0; b < AXL_9P_QID_LEN; b++) axl_9p_w_u8(&w, (uint8_t)(q*13+b)); }
    n = axl_9p_msg_finish(&w);
    test_check(n == 7 + 2 + 2 * AXL_9P_QID_LEN, "server-codec: Rwalk length = hdr + nwqid + 2*qid");
    axl_9p_r_init(&r, buf, n);
    axl_9p_msg_header(&r, &sz, &ty, &tag);
    test_check(axl_9p_r_u16(&r) == 2, "server-codec: Rwalk nwqid == 2");
}
```

- [ ] **Step 2: Run, confirm RED** — `make tests >/dev/null && TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh 2>&1 | grep -E "server-codec|Results:"`. Expect FAIL if any new constant is referenced that doesn't exist yet, else the test itself compiles (it only uses existing R-constants). If it passes already (all constants exist), still proceed to Step 3 to add the SETATTR/FSYNC constants needed by later tasks.

- [ ] **Step 3: Add the constants** — in `src/9p/axl-9p-internal.h` message-type enum, add:

```c
    AXL_9P_TSETATTR = 26,  AXL_9P_RSETATTR = 27,
    AXL_9P_TFSYNC   = 50,  AXL_9P_RFSYNC   = 51,
```

- [ ] **Step 4: Run, confirm GREEN** — same command; the `server-codec:` checks PASS.

- [ ] **Step 5: Commit** — `git add src/9p/axl-9p-internal.h test/unit/axl-test-9p.c && git commit -m "9p: server-direction reply codec test + Tsetattr/Tfsync message types"`

---

## Task 2: `Axl9pServer` skeleton — new/listen/free + accept loop + per-connection framing

The infrastructure: listen on a loop, accept clients, per-connection recv with size-prefix reassembly, and a dispatcher stub that replies `Rlerror(EPROTO)` to every message (handlers land in Tasks 3-5). This task's deliverable is: a client can connect, send a garbage/unknown message, and get a well-formed `Rlerror` back, and the connection reaps cleanly on close — proven by a minimal host client.

**Files:**
- Modify: `include/axl/axl-9p.h` (public API)
- Create: `src/9p/axl-9p-server.c`
- Modify: `Makefile` (add the source)

**Interfaces:**
- Consumes: `axl_tcp_listen` / `axl_tcp_accept_async` / `axl_tcp_recv_async` / `axl_tcp_send_async` / `axl_tcp_recv_get_size` / `axl_tcp_close` (`<axl/axl-tcp.h>`); `AxlLoop` (`<axl/axl-loop.h>`); codec (`axl-9p-internal.h`); `axl_calloc`/`axl_free`.
- Produces (public):
  - `typedef struct Axl9pServer Axl9pServer;`
  - `int axl_9p_server_new(AxlLoop *loop, const char *root, bool read_only, Axl9pServer **out);`
  - `int axl_9p_server_listen(Axl9pServer *s, uint16_t port);`  // 0 → default 564
  - `void axl_9p_server_free(Axl9pServer *s);`
- Produces (internal, file-local `static`): `S9pConn` (per-connection: `AxlTcp *sock; Axl9pServer *srv; uint8_t *rbuf; size_t rcap, rlen; uint8_t *msg; ...fid table...`), `s9p_on_accept`, `s9p_on_recv`, `s9p_dispatch(S9pConn*, const uint8_t *msg, size_t len)`, `s9p_send_rlerror(S9pConn*, uint16_t tag, uint32_t ecode)`, `s9p_reap(S9pConn*)`.

- [ ] **Step 1: Add the public API to `include/axl/axl-9p.h`**

```c
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
 * @return AXL_OK on success (@p out receives the server); AXL_ERR on NULL args
 *     or allocation failure.
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
 * @return AXL_OK on success; AXL_ERR if already listening or the listen fails.
 */
AXL_WARN_UNUSED int
axl_9p_server_listen(
    Axl9pServer *s,     ///< server from axl_9p_server_new
    uint16_t     port   ///< TCP port; 0 selects the 9P default (564)
);

/**
 * @brief Stop the server and free it. Reaps all live connections. NULL-safe.
 */
void
axl_9p_server_free(
    Axl9pServer *s   ///< server (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(Axl9pServer, axl_9p_server_free)
#endif
```

- [ ] **Step 2: Write `src/9p/axl-9p-server.c`** — the skeleton. Study `src/net/axl-http-server.c` for the accept/recv/reap shape and `sdk/examples/http-server-driver.c`/`echo-server.c` for the loop wiring. Implement:
  - `Axl9pServer { AxlLoop *loop; char root[256]; bool read_only; AxlTcp *listener; uint16_t port; S9pConn *conns; size_t max_conns; }` (a fixed conn pool like the HTTP server; default max_conns e.g. 8).
  - `S9pConn { AxlTcp *sock; Axl9pServer *srv; bool active; uint32_t msize; uint8_t *rbuf; size_t rcap; size_t rlen; ... }`. `rbuf` sized to the negotiated msize (start at `AXL_9P_MSIZE`, grow to the client-requested msize capped at a max e.g. 128 KiB on Tversion).
  - **Framing (the critical part):** `s9p_on_recv` appends received bytes to `conn->rbuf` at `rlen`; then loops: while `rlen >= 4`, read `size` (LE u32) from the front; if `size < 7 || size > rcap` → malformed → `s9p_reap`; if `rlen < size`, need more bytes → break (re-arm recv); else dispatch the complete `size`-byte message, then memmove the remainder down and continue. Re-arm `axl_tcp_recv_async` (return true) unless reaped.
  - `s9p_dispatch` (this task): parse the 7-byte header (`axl_9p_msg_header`); switch on type; for THIS task every case → `s9p_send_rlerror(conn, tag, 71 /*EPROTO*/)` EXCEPT it's fine to leave a `default`-only switch that rlerrors everything. (Tasks 3-5 replace the cases.)
  - `s9p_send_rlerror(conn, tag, ecode)`: build `Rlerror` into a small stack buffer, `axl_tcp_send_async` (or sync send) on `conn->sock`.
  - `s9p_reap(conn)`: close the socket, free `rbuf` + fid table, zero the slot.
  - `axl_9p_server_free`: reap all conns, close listener, free.

  Show the framing loop verbatim (it is the subtle part):

```c
/* Called with `n` new bytes already in conn->rbuf[conn->rlen..]. Consume as
   many complete messages as are buffered; re-arm recv for the rest. */
static bool
s9p_on_recv(AxlTcp *sock, AxlStatus status, void *data)
{
    S9pConn *conn = (S9pConn *)data;
    if (status != AXL_OK) {          /* peer closed or error */
        s9p_reap(conn);
        return false;                /* stop recv */
    }
    size_t n = axl_tcp_recv_get_size(sock);
    if (n == 0) {
        s9p_reap(conn);
        return false;
    }
    conn->rlen += n;
    for (;;) {
        if (conn->rlen < 4) {
            break;                   /* need the size prefix */
        }
        uint32_t msize = (uint32_t)conn->rbuf[0]
                       | ((uint32_t)conn->rbuf[1] << 8)
                       | ((uint32_t)conn->rbuf[2] << 16)
                       | ((uint32_t)conn->rbuf[3] << 24);
        if (msize < 7 || msize > conn->rcap) {
            s9p_reap(conn);          /* malformed / oversized */
            return false;
        }
        if (conn->rlen < msize) {
            break;                   /* partial message; wait for more */
        }
        s9p_dispatch(conn, conn->rbuf, msize);
        size_t rest = conn->rlen - msize;
        if (rest > 0) {
            axl_memmove(conn->rbuf, conn->rbuf + msize, rest);
        }
        conn->rlen = rest;
        if (!conn->active) {         /* dispatch reaped us (e.g. fatal) */
            return false;
        }
    }
    return true;                     /* re-arm recv into conn->rbuf + conn->rlen */
}
```

  NOTE: `axl_tcp_recv_async`'s buffer/offset handling — study its signature (`axl-tcp.h:424`) to recv into `conn->rbuf + conn->rlen` with capacity `conn->rcap - conn->rlen`; adapt the re-arm accordingly. If the async recv API always fills from offset 0, keep a separate accumulation buffer and copy. Match how `axl-http-conn.c` re-arms its recv.

- [ ] **Step 3: Wire the Makefile** — add `src/9p/axl-9p-server.c \` to the library source list (after `axl-9p-mount.c`). Build both arches: `make >/dev/null 2>&1 && echo X64_OK && make ARCH=aa64 >/dev/null 2>&1 && echo AA64_OK`.

- [ ] **Step 4: Minimal liveness test (defer full proof to Task 6)** — this task has no unit test (async socket server). The gate is: compiles + links both arches, and `axl_9p_server_new`/`_listen`/`_free` validate args. Add a tiny arg-contract unit check to `axl-test-9p.c` if feasible without a live socket (e.g. `axl_9p_server_new(NULL, "fs0:\\", false, &s) == AXL_ERR`); otherwise note it's covered by the Task 6 integration test.

- [ ] **Step 5: Commit** — `git add include/axl/axl-9p.h src/9p/axl-9p-server.c Makefile test/unit/axl-test-9p.c && git commit -m "9p: Axl9pServer skeleton — listen/accept/free + per-conn message framing + Rlerror dispatch"`

---

## Task 3: Fid table + handshake/navigation handlers (Tversion, Tattach, Twalk, Tclunk, Tgetattr)

**Files:**
- Modify: `src/9p/axl-9p-server.c`
- (No unit test; exercised in Task 6.)

**Interfaces:**
- Consumes: Task 2's `S9pConn` + dispatcher; codec; `AxlFs` `axl_file_info` (`<axl/axl-fs.h>`) for getattr; `axl_9p_client_split_parent`-style path join (or a local helper) to build child paths under `conn->srv->root`.
- Produces (file-local): the fid table API — `s9p_fid_set(conn, fid, const char *path, bool is_dir)`, `s9p_fid_get(conn, fid) -> S9pFid*` (or NULL), `s9p_fid_clunk(conn, fid)`; `S9pFid { uint32_t fid; bool used; bool is_dir; char path[512]; AxlDir *dir; ...open state... }`.

- [ ] **Step 1:** Add the fid table to `S9pConn` (a small fixed array, e.g. 64 fids, or a growable array; a hostile client opening unbounded fids must be capped → `Rlerror(EMFILE? use EBADF/ENOMEM)`. Cap at e.g. `AXL_9P_MAX_FIDS` 128).

- [ ] **Step 2:** Implement the handlers in `s9p_dispatch`, replacing the Task-2 blanket-rlerror for these types:
  - **Tversion** `msize[4] version[s]`: reply `Rversion(min(client_msize, server_cap), "9P2000.L")`; set `conn->msize`; resize `conn->rbuf` to the negotiated msize if larger. If the client version doesn't start with "9P2000" → reply `Rversion("unknown")` per spec (client then fails). Reset all fids (Tversion restarts a session).
  - **Tattach** `fid[4] afid[4] uname[s] aname[s] n_uname[4]`: bind `fid` → path = `conn->srv->root` (the export root); reply `Rattach(qid)` with the root's qid (build a qid from `axl_file_info` on root — type=dir).
  - **Twalk** `fid[4] newfid[4] nwname[2] wname[s]*`: resolve each component under the fid's path (reject `..` escaping the root; reject a non-dir mid-walk); bind `newfid` → the walked path; reply `Rwalk(nwqid, qid*)` where nwqid = number of components successfully walked (a partial walk still succeeds at the wire level, binding newfid to the last node — but per the client's expectation, return qids for each walked component). If ZERO components walk and the first is missing → `Rlerror(ENOENT)`. `newfid` may equal `fid` (walk-in-place) only if nwname==0.
  - **Tclunk** `fid[4]`: `s9p_fid_clunk`; reply `Rclunk`. Always succeeds (unknown fid → still `Rclunk`, or `Rlerror(EBADF)` — pick EBADF for unknown, matches diod).
  - **Tgetattr** `fid[4] request_mask[8]`: `axl_file_info(fid->path, &entry)`; reply `Rgetattr` with valid=request_mask&supported, qid, st_mode (S_IFDIR|0755 for dir, S_IFREG|0644 for file), size, mtime; zero the fields we don't track. Build the fixed Rgetattr body in the exact layout the client parser expects (Task 1 phase-3 established it).
  - Helper: `s9p_qid(const AxlFsEntry *e, uint8_t out[13])` — type byte (0x80=dir else 0x00), version[4]=0, path[8]= a stable hash of the path or the entry's identity (a simple FNV of the path is fine for v1; document it's not inode-stable across renames).

- [ ] **Step 3:** Build both arches clean.

- [ ] **Step 4: Commit** — `git add src/9p/axl-9p-server.c && git commit -m "9p server: fid table + Tversion/Tattach/Twalk/Tclunk/Tgetattr handlers"`

---

## Task 4: Read handlers (Tlopen, Tread, Treaddir)

**Files:** Modify: `src/9p/axl-9p-server.c`

**Interfaces:**
- Consumes: fid table (Task 3); `AxlFs` `axl_file_get_bytes`/`axl_file_writer_*` OR a positional read — study `<axl/axl-fs.h>` for a read-at-offset API; if only whole-file `axl_file_get_bytes` exists, read the whole file into the fid's open state once on Tlopen (bounded by a max) and serve slices from it (document the memory cost); prefer a positional file API if one exists (`axl_file_reader_*`? check). `axl_dir_open`/`axl_dir_read`/`axl_dir_close` for directories.
- Produces (file-local): open-state fields in `S9pFid` (`uint8_t *contents; size_t clen;` for a file opened read, or an `AxlDir *dir` for a directory + a readdir cursor).

- [ ] **Step 1: Tlopen** `fid[4] flags[4]`: if the fid is a directory, `axl_dir_open(fid->path, &fid->dir)`; if a file, open it for reading (positional reader if available, else lazily load on first Tread). Reply `Rlopen(qid, iounit=0)`.

- [ ] **Step 2: Tread** `fid[4] offset[8] count[4]`: clamp `count` to `conn->msize - 11`; read `count` bytes at `offset` from the file (positional read, or slice the loaded contents bounding `offset`/`count` against `clen`); reply `Rread(count, data)`. Offset ≥ size → count 0 (EOF). NEVER read past the source buffer; NEVER write more than `count` into the reply.

- [ ] **Step 3: Treaddir** `fid[4] offset[8] count[4]`: iterate `axl_dir_read` producing 9P dirent records (`qid[13] offset[8] type[1] name[s]`) into the reply up to `count` bytes; the `offset` is the dirent cursor (0 = start; the client passes back the last emitted dirent offset). Include synthetic `.`/`..` first (offset 1 and 2) so `mount -t 9p` sees them, then real entries at offsets ≥3. Reply `Rreaddir(count, data)`; count 0 = end.

- [ ] **Step 4:** Build both arches clean.

- [ ] **Step 5: Commit** — `git add src/9p/axl-9p-server.c && git commit -m "9p server: Tlopen/Tread/Treaddir read handlers over AxlFs"`

---

## Task 5: Write handlers (Tlcreate, Twrite, Tmkdir, Tremove, Trename, Tsetattr, Tfsync) + read-only gating

**Files:** Modify: `src/9p/axl-9p-server.c`

**Interfaces:**
- Consumes: fid table + `AxlFs` `axl_file_set_contents`/`axl_file_writer_open`/`_write`/`_close`, `axl_dir_mkdir`, `axl_file_delete`/`axl_dir_rmdir`, `axl_file_rename`.

- [ ] **Step 1: read-only gate** — at the top of `s9p_dispatch`, for the mutating types, if `conn->srv->read_only` → `s9p_send_rlerror(conn, tag, 30 /*EROFS*/)` and return.
- [ ] **Step 2: Tlcreate** `dfid[4] name[s] flags[4] mode[4] gid[4]`: create `dfid->path + "/" + name` as a file (`axl_file_set_contents(path, "", 0)` or a writer), rebind `dfid` → the new file (per 9P .L, Tlcreate turns dfid into the open new file), reply `Rlcreate(qid, iounit=0)`.
- [ ] **Step 3: Twrite** `fid[4] offset[8] count[4] data[count]`: bound `count` so `count` bytes are actually present in the message (offset into the msg is 7+4+8+4=23; `data` must be within `size`); write at `offset` (positional writer; or read-modify-write via whole-file if no positional API — document); reply `Rwrite(count)`.
- [ ] **Step 4: Tmkdir** `dfid[4] name[s] mode[4] gid[4]`: `axl_dir_mkdir(dfid->path + "/" + name)`; reply `Rmkdir(qid)`.
- [ ] **Step 5: Tremove** `fid[4]`: remove the fid's path (`axl_file_delete` or `axl_dir_rmdir` per is_dir); clunk the fid (Tremove always clunks); reply `Rremove` (or `Rlerror` on failure — still clunk).
- [ ] **Step 6: Trename** `fid[4] dfid[4] name[s]`: `axl_file_rename(fid->path, dfid->path + "/" + name)`; update the fid's stored path; reply `Rrename`.
- [ ] **Step 7: Tsetattr** `fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime*[16] mtime*[16]`: honor a size change (truncate/extend the file to `size` when `valid & ATTR_SIZE(0x8)`); ignore mode/uid/gid/times (accept as no-op); reply `Rsetattr`. **Tfsync** `fid[4] datasync[4]`: reply `Rfsync` (writes are already synchronous). Each errno-maps failures.
- [ ] **Step 8:** Build both arches clean.
- [ ] **Step 9: Commit** — `git add src/9p/axl-9p-server.c && git commit -m "9p server: write handlers (create/write/mkdir/remove/rename/setattr/fsync) + read-only gating"`

---

## Task 6: Integration test — host 9P client mounts the guest server (+ manual kernel-mount doc)

**Files:**
- Create: `test/integration/p9-client.py`
- Create: `test/integration/9p-server-selftest.c`
- Create: `test/integration/test-9p-server-qemu.sh`
- Modify: `Makefile` (add `9p-server-selftest` target)

**Interfaces:**
- Consumes: `axl_9p_server_new`/`_listen`/`_free`, `axl_loop_new`/`axl_loop_run` or the loop-iterate API, `axl_ramdisk_create` (seed a known tree) or serve `fs0:\`; `AXL_APP`.

- [ ] **Step 1: `p9-client.py`** — a minimal host 9P2000.L client: `connect(host,port)`, `version`, `attach`, `walk(path)`, `lopen`, `read`, `readdir`, `write`, `getattr`, `mkdir`, `remove`. Mirror the wire layout in `p9-server.py` (reuse its constants/helpers structure). It prints machine-greppable lines the harness asserts.

- [ ] **Step 2: `9p-server-selftest.c`** — guest app: create a RAM disk labeled e.g. `NINEP` (`axl_ramdisk_create`), write `hello.txt` = `"hello from 9p server\n"` and a `sub/` dir into it via `axl_file_set_contents`/`axl_dir_mkdir`; `axl_9p_server_new(loop, "<ramdisk-vol>:\\", false, &srv)`; `axl_9p_server_listen(srv, GUEST_PORT)`; print `SERVER: LISTENING port=<n>`; pump the loop (`axl_loop_run` with a deadline, or iterate until a stop file/timeout). If the RAM-disk protocol is absent (aa64/AAVMF), fall back to serving `fs0:\` or print `SERVER: SKIP no-ramdisk` and exit 0 (balance the harness).

- [ ] **Step 3: `test-9p-server-qemu.sh`** — mirror `test-http.sh`: `HOST_PORT=$(test_port 0)`, `GUEST_PORT=<fixed>`, `test_add_port_forward "$HOST_PORT" "$GUEST_PORT"`, boot `9p-server-selftest.efi` in the background with `--net`, wait for `SERVER: LISTENING`, then run `python3 p9-client.py 127.0.0.1 "$HOST_PORT"` and assert exact lines:

```bash
python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" > "$CLIENT_OUT" 2>&1
grep -q "READ hello.txt = hello from 9p server" "$CLIENT_OUT" || { echo "FAIL: read"; exit 1; }
grep -q "READDIR contains hello.txt" "$CLIENT_OUT" || { echo "FAIL: readdir"; exit 1; }
grep -q "READDIR contains sub" "$CLIENT_OUT" || { echo "FAIL: readdir sub"; exit 1; }
grep -q "WRITE+READBACK ok" "$CLIENT_OUT" || { echo "FAIL: write"; exit 1; }
grep -q "MKDIR+READDIR ok" "$CLIENT_OUT" || { echo "FAIL: mkdir"; exit 1; }
echo "PASS: 9p server round-trip"
```

- [ ] **Step 4: Makefile target** — add `9p-server-selftest` mirroring an existing `*-selftest` recipe (with `--net`/network libs if the selftest needs them; check how other net selftests build).

- [ ] **Step 5: Run** — `make 9p-server-selftest >/dev/null && ./test/integration/test-9p-server-qemu.sh 2>&1 | tail -20`. Expect `PASS: 9p server round-trip`. If a guest-listener-reachable-from-host proves impossible in this harness, STOP and report BLOCKED with specifics (do NOT fake it).

- [ ] **Step 6: Document the kernel mount** — in `src/9p/README.md`, add the real-hardware/manual validation: `mount -t 9p -otrans=tcp,port=<n>,version=9p2000.L,msize=8192 <ip> /mnt` (needs root + the kernel `9p`/`9pnet_tcp` modules), noting it's the same wire the automated Python-client test exercises.

- [ ] **Step 7: Commit** — `git add test/integration/p9-client.py test/integration/9p-server-selftest.c test/integration/test-9p-server-qemu.sh Makefile src/9p/README.md && git commit -m "9p server: host-client integration test (mount round-trip) + kernel-mount doc"`

---

## Task 7: Both-arch validation, docs, review, final commit

**Files:** Modify: `docs/sphinx/modules/9p.rst`, `src/9p/README.md`, `test/integration/.last-pass-count`

- [ ] **Step 1: Both-arch unit suite** — `make tests >/dev/null && ./test/integration/test-axl.sh 2>&1 | tail -1`; `make ARCH=aa64 tests >/dev/null && ./test/integration/test-axl.sh --arch AARCH64 2>&1 | tail -1`. Both pass, equal counts.
- [ ] **Step 2: Gates** — `make check-ascii 2>&1 | tail -1`; `make check-docs 2>&1 | tail -1`; both clean.
- [ ] **Step 3: Doc sync** — `src/9p/README.md` "Server" section (`axl_9p_server_new`/`_listen`/`_free`; runs on the caller's loop; read_only; the export root; the fid/qid caveats); `9p.rst` renders the new functions (existing `doxygenfile`). `./scripts/build-docs.sh 2>&1 | grep -iE "error|9p" | tail`.
- [ ] **Step 4: Independent pre-commit review** — dispatch a review over the whole server diff: focus on (a) recv framing can't overrun `rbuf` on any hostile size/partial-message sequence; (b) every Tread/Twrite/Treaddir bounds count/offset against both the message and the backing file/buffer; (c) fid table can't be exhausted to fault (cap + Rlerror); (d) connection reap frees rbuf + fid table + any open AxlDir/loaded contents (no leak on drop mid-op); (e) read_only gates ALL mutating ops; (f) path resolution rejects `..` escaping the export root; (g) no `(void)`-cast on must-check returns. Apply fixes.
- [ ] **Step 5: Final commit + mark Phase 4 done** in `docs/superpowers/specs/2026-07-19-axl-9p-design.md` §12.

```bash
git add docs/sphinx/modules/9p.rst src/9p/README.md test/integration/.last-pass-count docs/superpowers/specs/2026-07-19-axl-9p-design.md
git commit -m "9p server: docs + both-arch validation (Phase 4)"
```

---

## Self-Review Notes (author)

- **Spec coverage:** §5 (server on AxlLoop+AxlTcp, AxlFs backend) → Tasks 2-5; §6 API (`axl_9p_server_new`/`_listen`/`_free`) → Task 2; §7 messages (all T→R handlers + Rlerror) → Tasks 3-5; §9 error handling (Rlerror errno map, connection reap, --ro EROFS, no downgrade) → Tasks 2/3/5; §10 server test (headline `mount -t 9p`) → Task 6, realized as an automated host **Python 9P client** over `test_add_port_forward` (portable, no root) with the kernel `mount -t 9p` documented as the manual/real-HW equivalent (same wire) — a deliberate, documented deviation from "kernel mount in CI" because kernel mount needs root + the 9p modules, which CI lacks; the Python client exercises the identical protocol.
- **Placeholder scan:** the framing loop, public API, and test assertions are concrete. The handler tasks specify exact wire layouts + the AxlFs calls; where the AxlFs positional-read/write API name is uncertain, the task says to check `<axl/axl-fs.h>` and gives the whole-file fallback — this is a real instruction, not a placeholder, because the exact reader API must be confirmed against the header at implementation time.
- **Type consistency:** `Axl9pServer`/`axl_9p_server_new(loop, root, read_only, out)`/`_listen(s, port)`/`_free(s)` identical across header (Task 2) + test (Task 6). `S9pConn`/`S9pFid` are file-local, introduced in Task 2/3 and extended in 4/5.
- **Sizing:** `src/9p/axl-9p-server.c` may approach ~900 lines with all handlers; the plan flags splitting handlers into `axl-9p-server-ops.c` if it grows unwieldy (report DONE_WITH_CONCERNS).
- **Known v1 limits (documented):** qid.path is a path hash (not inode-stable across rename); one request in flight per connection; positional write may be whole-file read-modify-write if AxlFs lacks a positional writer (memory cost noted). All acceptable per spec §4/§13.
