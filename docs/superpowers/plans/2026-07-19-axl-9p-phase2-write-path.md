# Axl9p Phase 2 — Client Write Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the 9P2000.L client write path — `axl_9p_write_file`, `axl_9p_mkdir`, `axl_9p_remove`, `axl_9p_rename` — over the existing read-only Phase 1 client, with chunked large writes.

**Architecture:** Extend the existing synchronous client in `src/9p/axl-9p-client.c` (built on `AxlTcp` + `axl_9p_transact`). Each op is a small `Twalk`→op→`Tclunk` sequence mirroring the Phase 1 read/list functions. Five new wire messages (`Tlcreate`, `Twrite`, `Tmkdir`, `Tremove`, `Trename`) are constructed inline with the existing `axl_9p_w_*` writer helpers + one new raw-bytes helper. The Phase 1 integration server (`p9-server.py`, read-only today) is extended first (infra-first) so the client's writes have a peer to round-trip against.

**Tech Stack:** C (AXL public types only in the header — `Axl9pClient`, `AxlBytes`, standard C types), the internal `src/9p/axl-9p-internal.h` codec, QEMU integration via `test-9p-qemu.sh` against a host Python 9P server.

## Global Constraints

- **Public API uses standard C types + snake_case + UTF-8** — no EDK2/UEFI types leak through `<axl/axl-9p.h>` (`docs/AXL-Coding-Style.md`).
- **Doc comments:** `///<` inline params, `@brief`/`@return` in the block comment; multi-line signatures even single-param.
- **Test-first, both arches (X64 + AARCH64), independent pre-commit review** are mandatory (`CLAUDE.md`). Direct commits to `main`, solo. Do NOT push without asking.
- **Exact-string / byte-exact assertions** for codec + output (`axl_strcmp`/byte compares, never substring).
- **Integration tests** opt out of the ratchet: `export TEST_SKIP_RATCHET=1`; `# test-meta: arch=both` runs both arches. Unit binary `AxlTest9p` IS ratcheted.
- **9P2000.L wire:** little-endian; header `size[4] type[1] tag[2]`; `string = len[2]+utf8`; `qid = type[1] version[4] path[8]` (13 bytes). `Rlerror(type=7)` carries a Linux errno. `msize` negotiated (`AXL_9P_MSIZE` floor); reads/writes chunk to `msize - header`.
- **Linux open flags** (numeric, as 9P2000.L carries them): `O_WRONLY=1`, `O_CREAT=0x40`, `O_TRUNC=0x200`. `O_WRONLY|O_TRUNC = 0x201`.
- **New 9P2000.L message type numbers:** `Tlcreate=14/Rlcreate=15`, `Trename=20/Rrename=21`, `Tmkdir=72/Rmkdir=73`, `Twrite=118/Rwrite=119`, `Tremove=122/Rremove=123`.

---

## File Structure

- `test/integration/p9-server.py` — **modify**: add write-op handlers (`Tlcreate`, `Twrite`, `Tmkdir`, `Tremove`, `Trename`) + honor `O_TRUNC` in `Tlopen`. Infra for Tasks 3-5.
- `src/9p/axl-9p-internal.h` — **modify**: 5 new message-type enum values; declare `axl_9p_w_bytes`.
- `src/9p/axl-9p-codec.c` — **modify**: implement `axl_9p_w_bytes`.
- `src/9p/axl-9p-client.c` — **modify**: `client_split_parent` helper; the 4 new public functions.
- `include/axl/axl-9p.h` — **modify**: 4 new public declarations + docstrings (the contract).
- `test/unit/axl-test-9p.c` — **modify**: byte-exact `axl_9p_w_bytes` test + message-type sanity.
- `test/unit/axl-test-net.c` — **modify**: the `9p-client` mode (`run_9p_client_mode`, driven by `AxlTestNet.efi 9p-client <host> <port>`) is the client harness — append the write/mkdir/remove/rename round-trips to its existing connect→read→list sequence, emitting `WRITE-RB:` / `MKDIR-OK:` / `REMOVE-GONE:` / `RENAME-RB:` markers.
- `test/integration/test-9p-qemu.sh` — **modify**: grep-assert those markers in `$TEST_CLEAN_LOG`.
- `src/9p/README.md` — **modify**: document the write path.

---

## Task 1: Extend `p9-server.py` with the write ops (infra-first)

Bucket E (new test infra): write the infra, confirm existing read tests still pass, THEN TDD the client on top. The server is a host-side Python 9P2000.L server backed by a temp directory; Phase 1 implemented the read-only subset. This task adds the write handlers so Tasks 3-5 have a peer.

**Files:**
- Modify: `test/integration/p9-server.py`

**Interfaces:**
- Consumes: the existing dispatch loop + `put_str`/`get_str`/`qid_for`/fid table (read the file first — reuse its exact helper names).
- Produces: server that answers `Tlcreate(14)`, `Twrite(118)`, `Tmkdir(72)`, `Tremove(122)`, `Trename(20)`, and honors `O_TRUNC` in `Tlopen(12)`.

- [ ] **Step 1: Read the existing server to learn its helpers**

Run: `sed -n '1,140p' test/integration/p9-server.py`
Note the exact names of: the message-type constants block, the fid→path map, `put_str`, the `Rlerror` sender, and how `Tlopen`/`Tread`/`Treaddir` are dispatched. Match these names below (the code blocks here assume `fids: dict[int,str]`, `put_str(s)`, `send(tag, rtype, body)`, `rlerror(tag, errno)` — **rename to match the actual file** if they differ).

- [ ] **Step 2: Add the new message-type constants**

Near the existing constants (after `TCLUNK, RCLUNK = 120, 121`):

```python
TLCREATE, RLCREATE = 14, 15
TRENAME,  RRENAME  = 20, 21
TMKDIR,   RMKDIR   = 72, 73
TWRITE,   RWRITE   = 118, 119
TREMOVE,  RREMOVE  = 122, 123

O_TRUNC = 0x200
```

- [ ] **Step 3: Honor O_TRUNC in the existing Tlopen handler**

In the `Tlopen` branch, after resolving the fid's path, truncate when the flag is set (find the line that unpacks `fid, flags`):

```python
elif mtype == TLOPEN:
    fid, flags = struct.unpack("<II", body[:8])
    path = fids[fid]
    if flags & O_TRUNC and os.path.isfile(path):
        open(path, "wb").close()          # truncate to zero
    # ...existing qid + iounit reply unchanged...
```

- [ ] **Step 4: Add the write-op handlers**

Add these branches to the dispatch (each reads `body`, mutates the temp dir, replies). `Tlcreate` changes `fid` into the newly created file (matching 9P semantics):

```python
elif mtype == TLCREATE:                    # fid[4] name[s] flags[4] mode[4] gid[4]
    off = 0
    fid, = struct.unpack("<I", body[off:off+4]); off += 4
    name, off = get_str(body, off)
    flags, mode, gid = struct.unpack("<III", body[off:off+12])
    newpath = os.path.join(fids[fid], name)
    fd = os.open(newpath, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, mode & 0o777)
    os.close(fd)
    fids[fid] = newpath                    # fid now refers to the new file
    q = qid_for(newpath)
    send(tag, RLCREATE, q + struct.pack("<I", 0))          # qid[13] iounit[4]

elif mtype == TWRITE:                       # fid[4] offset[8] count[4] data[count]
    fid, offset, count = struct.unpack("<IQI", body[:16])
    data = body[16:16+count]
    with open(fids[fid], "r+b") as f:
        f.seek(offset); f.write(data)
    send(tag, RWRITE, struct.pack("<I", len(data)))        # count[4]

elif mtype == TMKDIR:                        # dfid[4] name[s] mode[4] gid[4]
    off = 0
    dfid, = struct.unpack("<I", body[off:off+4]); off += 4
    name, off = get_str(body, off)
    mode, gid = struct.unpack("<II", body[off:off+8])
    newpath = os.path.join(fids[dfid], name)
    os.mkdir(newpath, mode & 0o777)
    send(tag, RMKDIR, qid_for(newpath))                    # qid[13]

elif mtype == TREMOVE:                        # fid[4]  (removes + clunks the fid)
    fid, = struct.unpack("<I", body[:4])
    path = fids.pop(fid)
    (os.rmdir if os.path.isdir(path) else os.remove)(path)
    send(tag, RREMOVE, b"")

elif mtype == TRENAME:                         # fid[4] dfid[4] name[s]
    fid, dfid = struct.unpack("<II", body[:8])
    name, _ = get_str(body, 8)
    os.rename(fids[fid], os.path.join(fids[dfid], name))
    fids[fid] = os.path.join(fids[dfid], name)
    send(tag, RRENAME, b"")
```

> If the actual server catches `OSError` centrally and maps to `Rlerror`, wrap these in that existing pattern instead of letting the exception kill the connection — check how the read handlers report a missing file and mirror it. If there is no central handler, wrap each branch body in `try/except OSError as e: rlerror(tag, e.errno)`.

- [ ] **Step 5: Confirm the infra didn't break the Phase 1 read tests**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -20`
Expected: the existing Phase 1 read/list assertions still PASS (this task added branches only; the read path is untouched). If the server now throws on startup (syntax), fix before proceeding.

- [ ] **Step 6: Commit**

```bash
git add test/integration/p9-server.py
git commit -m "9p(test): p9-server.py write ops (Tlcreate/Twrite/Tmkdir/Tremove/Trename)"
```

---

## Task 2: Codec — message-type constants + `axl_9p_w_bytes` (byte-exact)

Bucket B (exact bytes). The 4 client functions build their T-messages with `axl_9p_w_*`; `Twrite` needs to append a raw data payload, for which there is no helper yet. Add `axl_9p_w_bytes` and pin its bytes.

**Files:**
- Modify: `src/9p/axl-9p-internal.h` (enum + declaration)
- Modify: `src/9p/axl-9p-codec.c` (implementation)
- Test: `test/unit/axl-test-9p.c`

**Interfaces:**
- Produces: `void axl_9p_w_bytes(Axl9pWriter *w, const void *data, size_t len);` — appends `len` raw bytes, setting `w->overflow` if they don't fit (same overflow discipline as the other `w_*` helpers).
- Produces: enum values `AXL_9P_TLCREATE/RLCREATE`, `AXL_9P_TRENAME/RRENAME`, `AXL_9P_TMKDIR/RMKDIR`, `AXL_9P_TWRITE/RWRITE`, `AXL_9P_TREMOVE/RREMOVE`.

- [ ] **Step 1: Write the failing test**

In `test/unit/axl-test-9p.c`, add (find the codec-primitive test group — near the `w_str` / golden-bytes tests):

```c
static void test_9p_w_bytes(void) {
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
```

Register it in the test runner list the same way the neighboring codec tests are registered (grep the file for how `test_9p_` functions are invoked in `main`).

- [ ] **Step 2: Run to verify it fails**

Run: `TEST_APPS_ONLY=AxlTest9p TEST_SKIP_RATCHET=1 timeout 90 ./test/integration/test-axl.sh --arch X64 2>&1 | tail -20`
Expected: FAIL to compile ("implicit declaration of `axl_9p_w_bytes`") — that IS the red for a missing symbol.

- [ ] **Step 3: Add the enum values**

In `src/9p/axl-9p-internal.h`, extend the message-type enum (after `AXL_9P_TCLUNK = 120, AXL_9P_RCLUNK = 121,`):

```c
    AXL_9P_TLCREATE = 14,  AXL_9P_RLCREATE = 15,
    AXL_9P_TRENAME  = 20,  AXL_9P_RRENAME  = 21,
    AXL_9P_TMKDIR   = 72,  AXL_9P_RMKDIR   = 73,
    AXL_9P_TWRITE   = 118, AXL_9P_RWRITE   = 119,
    AXL_9P_TREMOVE  = 122, AXL_9P_RREMOVE  = 123,
```

- [ ] **Step 4: Declare + implement `axl_9p_w_bytes`**

Declare in `src/9p/axl-9p-internal.h` (after `axl_9p_w_str`):

```c
void   axl_9p_w_bytes(Axl9pWriter *w, const void *data, size_t len);
```

Implement in `src/9p/axl-9p-codec.c` (mirror the overflow discipline of `axl_9p_w_str` — read it first for the exact `w->overflow` / `w->len` / `w->cap` field names):

```c
void
axl_9p_w_bytes(Axl9pWriter *w, const void *data, size_t len)
{
    if (w->len + len > w->cap) {
        w->overflow = true;
        return;
    }
    axl_memcpy(w->buf + w->len, data, len);
    w->len += len;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `TEST_APPS_ONLY=AxlTest9p TEST_SKIP_RATCHET=1 timeout 90 ./test/integration/test-axl.sh --arch X64 2>&1 | tail -20`
Expected: PASS, the AxlTest9p count up by the new checks.

- [ ] **Step 6: Commit**

```bash
git add src/9p/axl-9p-internal.h src/9p/axl-9p-codec.c test/unit/axl-test-9p.c
git commit -m "9p: codec axl_9p_w_bytes + write-path message-type constants"
```

---

## Task 3: `axl_9p_write_file` (walk-or-create + chunked Twrite)

Contract-first (header docstring) then RED integration round-trip then implement. The design was set with the user in the spec (§6 API + §5 mapping table), so no separate contract review — the header docstring below IS the contract.

**Files:**
- Modify: `include/axl/axl-9p.h` (declaration + docstring)
- Modify: `src/9p/axl-9p-internal.h` (add a `wbuf` send buffer to the client struct)
- Modify: `src/9p/axl-9p-client.c` (helper + function)
- Test: `test/integration/test-9p-qemu.sh`

**Interfaces:**
- Consumes: `client_walk(c, path, &fid)`, `client_lopen(c, fid, flags)`, `client_clunk(c, fid)`, `axl_9p_transact`, `axl_9p_w_bytes` (Task 2).
- Produces:
  - `int axl_9p_write_file(Axl9pClient *c, const char *path, const void *buf, size_t len);`
  - static `int client_split_parent(const char *path, char *parent, size_t cap, const char **name_out);` — splits `"/a/b/c"` into parent `"/a/b"` + name `"c"`; root-level `"/c"` yields parent `"/"`.
  - static `int client_lcreate(Axl9pClient *c, uint32_t dfid, const char *name, uint32_t flags, uint32_t mode);` — `Tlcreate`; `dfid` becomes the open new file.

- [ ] **Step 1: Write the header contract (docstring)**

In `include/axl/axl-9p.h`, after `axl_9p_read_file`:

```c
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
```

- [ ] **Step 2: Add the RED integration assertion**

The client harness is the `9p-client` mode in `test/unit/axl-test-net.c` (`run_9p_client_mode`), which already connects + reads + lists. Append a write→read-back step to that sequence (after the existing list step, before it prints `9P-CLIENT-OK`), then grep for its marker in `test/integration/test-9p-qemu.sh`:

```bash
# write a new file, read it back
grep -q "WRITE-RB: hello-9p-write" "$TEST_CLEAN_LOG" \
    && pass "write_file round-trips (create)" || fail "write_file create"
```

Where the harness verb does:
```c
/* pseudo-contract for the harness 'write' verb */
axl_9p_write_file(c, "/wtest.txt", "hello-9p-write", 14);
AxlBytes *b = NULL;
axl_9p_read_file(c, "/wtest.txt", &b);
axl_printf("WRITE-RB: %.*s\n", (int)axl_bytes_len(b), (const char *)axl_bytes_data(b));
axl_bytes_unref(b);
```

- [ ] **Step 3: Run to verify it fails**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: FAIL — `axl_9p_write_file` undefined (link error) or the assertion missing. Confirm RED before implementing.

- [ ] **Step 4: Add the `wbuf` send buffer to the client struct**

In `src/9p/axl-9p-internal.h`, add to `struct Axl9pClient` (a Twrite request is header+params+chunk ≤ msize; mirror `rbuf`):

```c
    uint8_t  wbuf[AXL_9P_MSIZE];   ///< scratch for building a Twrite request
```

- [ ] **Step 5: Implement the helpers + `axl_9p_write_file`**

In `src/9p/axl-9p-client.c` (place the statics above the public function; mirror `client_lopen` style):

```c
/* Split "/a/b/c" -> parent "/a/b", name "c". Root-level "/c" -> parent "/".
   Returns AXL_ERR on no '/' or a trailing-'/' (no basename). */
static int
client_split_parent(const char *path, char *parent, size_t cap,
                    const char **name_out)
{
    const char *slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') slash = p;
    }
    if (slash == NULL || slash[1] == '\0') {
        return AXL_ERR;                       /* not absolute, or no basename */
    }
    size_t plen = (size_t)(slash - path);
    if (plen == 0) plen = 1;                  /* "/c" -> parent "/" */
    if (plen + 1 > cap) {
        return AXL_ERR;
    }
    axl_memcpy(parent, path, plen);
    parent[plen] = '\0';
    *name_out = slash + 1;
    return AXL_OK;
}

/* Tlcreate(dfid, name, flags, mode, gid=0) -> Rlcreate. dfid becomes the
   newly created, open file. */
static int
client_lcreate(Axl9pClient *c, uint32_t dfid, const char *name,
               uint32_t flags, uint32_t mode)
{
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TLCREATE, 0);
    axl_9p_w_u32(&w, dfid);
    axl_9p_w_str(&w, name);
    axl_9p_w_u32(&w, flags);
    axl_9p_w_u32(&w, mode);
    axl_9p_w_u32(&w, 0);                        /* gid */
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RLCREATE);
}

int
axl_9p_write_file(Axl9pClient *c, const char *path, const void *buf, size_t len)
{
    if (c == NULL || path == NULL || (buf == NULL && len != 0)) {
        return AXL_ERR;
    }

    /* Resolve the write fid: truncate-open an existing file, else create it. */
    uint32_t fid;
    if (client_walk(c, path, &fid) == AXL_OK) {
        if (client_lopen(c, fid, 0x201 /* O_WRONLY|O_TRUNC */) != AXL_OK) {
            client_clunk(c, fid);
            return AXL_ERR;
        }
    } else {
        char parent[512];
        const char *name = NULL;
        if (client_split_parent(path, parent, sizeof(parent), &name) != AXL_OK
            || client_walk(c, parent, &fid) != AXL_OK) {
            return AXL_ERR;
        }
        /* Tlcreate turns `fid` into the new open file. */
        if (client_lcreate(c, fid, name, 0x201 /* O_WRONLY|O_TRUNC */,
                           0644) != AXL_OK) {
            client_clunk(c, fid);
            return AXL_ERR;
        }
    }

    /* Chunked Twrite(fid, offset, count, data). Chunk = msize - (header7 +
       fid4 + offset8 + count4) = msize - 23. */
    int rc = AXL_OK;
    uint64_t offset = 0;
    size_t   remaining = len;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t maxchunk = c->msize - 23;
    while (remaining > 0) {
        uint32_t n = (remaining < maxchunk) ? (uint32_t)remaining : maxchunk;
        Axl9pWriter w;
        axl_9p_msg_begin(&w, c->wbuf, sizeof(c->wbuf), AXL_9P_TWRITE, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, n);
        axl_9p_w_bytes(&w, src, n);
        if (w.overflow
            || axl_9p_transact(c, c->wbuf, axl_9p_msg_finish(&w),
                               AXL_9P_RWRITE) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t wrote = axl_9p_r_u32(&r);      /* Rwrite: count[4] */
        if (r.error || wrote == 0 || wrote > n) {
            rc = AXL_ERR;                        /* short/❌ write */
            break;
        }
        offset    += wrote;
        src       += wrote;
        remaining -= wrote;
    }

    client_clunk(c, fid);
    return rc;
}
```

> Verify before implementing: confirm `Axl9pReader` exposes `.error` (Phase 1 uses `r.error` in `client_walk`) and that `axl_9p_msg_finish` patches `size[4]` in place (Phase 1's reads rely on it). Both are used identically in the read path above.

- [ ] **Step 6: Run to verify it passes**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: PASS "write_file round-trips (create)".

- [ ] **Step 7: Refactor while green**

Re-read `axl_9p_write_file` + the two helpers for duplication vs the Phase 1 read path (the walk/clunk framing should match; if the chunk-arithmetic magic number `23` reads poorly, name it). Re-run Step 6 after any change.

- [ ] **Step 8: Commit**

```bash
git add include/axl/axl-9p.h src/9p/axl-9p-internal.h src/9p/axl-9p-client.c test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_write_file (walk-or-create + chunked Twrite)"
```

---

## Task 4: `axl_9p_mkdir` + `axl_9p_remove`

Two small ops sharing the parent-split helper. `Tmkdir` creates under a parent dfid; `Tremove` removes the node at a fid AND clunks it server-side (so no client clunk after).

**Files:**
- Modify: `include/axl/axl-9p.h`
- Modify: `src/9p/axl-9p-client.c`
- Test: `test/integration/test-9p-qemu.sh`

**Interfaces:**
- Consumes: `client_walk`, `client_split_parent` (Task 3), `client_clunk`, `axl_9p_transact`.
- Produces: `int axl_9p_mkdir(Axl9pClient *c, const char *path);` and `int axl_9p_remove(Axl9pClient *c, const char *path);`

- [ ] **Step 1: Header contracts**

```c
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
```

- [ ] **Step 2: RED integration assertions**

Append `mkdir` and `remove` steps to `run_9p_client_mode` in `test/unit/axl-test-net.c`, and grep their markers in `test/integration/test-9p-qemu.sh`:

```bash
# mkdir then list shows the new dir; write+remove then read must fail
grep -q "MKDIR-OK: /newdir" "$TEST_CLEAN_LOG"  && pass "mkdir creates a dir"      || fail "mkdir"
grep -q "REMOVE-GONE: /wtest.txt" "$TEST_CLEAN_LOG" && pass "remove deletes a file" || fail "remove"
```

Harness verbs:
```c
/* mkdir: create then confirm via list */
if (axl_9p_mkdir(c, "/newdir") == AXL_OK) axl_printf("MKDIR-OK: /newdir\n");

/* remove: delete the file Task 3 wrote, confirm read now fails */
axl_9p_remove(c, "/wtest.txt");
AxlBytes *b = NULL;
if (axl_9p_read_file(c, "/wtest.txt", &b) != AXL_OK) axl_printf("REMOVE-GONE: /wtest.txt\n");
else axl_bytes_unref(b);
```

- [ ] **Step 3: Run to verify it fails**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: FAIL (undefined `axl_9p_mkdir`/`axl_9p_remove`).

- [ ] **Step 4: Implement**

In `src/9p/axl-9p-client.c`:

```c
int
axl_9p_mkdir(Axl9pClient *c, const char *path)
{
    if (c == NULL || path == NULL) {
        return AXL_ERR;
    }
    char parent[512];
    const char *name = NULL;
    if (client_split_parent(path, parent, sizeof(parent), &name) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t dfid;
    if (client_walk(c, parent, &dfid) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TMKDIR, 0);
    axl_9p_w_u32(&w, dfid);          /* dfid */
    axl_9p_w_str(&w, name);          /* name */
    axl_9p_w_u32(&w, 0755);          /* mode */
    axl_9p_w_u32(&w, 0);             /* gid  */
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RMKDIR);
    client_clunk(c, dfid);
    return rc;
}

int
axl_9p_remove(Axl9pClient *c, const char *path)
{
    if (c == NULL || path == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREMOVE, 0);
    axl_9p_w_u32(&w, fid);
    /* Tremove clunks `fid` server-side on BOTH success and error — do NOT
       client_clunk it afterward (that would be a double-clunk / bad-fid). */
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREMOVE);
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: PASS "mkdir creates a dir" + "remove deletes a file".

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-9p.h src/9p/axl-9p-client.c test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_mkdir + axl_9p_remove"
```

---

## Task 5: `axl_9p_rename`

`Trename(fid, dfid, name)` — move the node at `fid` into directory `dfid` under `name`.

**Files:**
- Modify: `include/axl/axl-9p.h`
- Modify: `src/9p/axl-9p-client.c`
- Test: `test/integration/test-9p-qemu.sh`

**Interfaces:**
- Consumes: `client_walk`, `client_split_parent`, `client_clunk`, `axl_9p_transact`.
- Produces: `int axl_9p_rename(Axl9pClient *c, const char *from, const char *to);`

- [ ] **Step 1: Header contract**

```c
/**
 * @brief Rename / move a file or directory on the server.
 *
 * Walks to @p from and to @p to's parent directory (which must exist), then
 * moves @p from to @p to's final component within it. Both endpoints are on
 * the same server session.
 *
 * @return AXL_OK on success; AXL_ERR if @p from is missing, @p to's parent is
 *     missing, or on NULL args.
 */
int
axl_9p_rename(
    Axl9pClient *c,      ///< connected session
    const char  *from,   ///< existing absolute path
    const char  *to      ///< destination absolute path
);
```

- [ ] **Step 2: RED integration assertion**

Append a `rename` step to `run_9p_client_mode` in `test/unit/axl-test-net.c` + grep assertion in `test/integration/test-9p-qemu.sh`:

```bash
grep -q "RENAME-RB: hello-9p-write" "$TEST_CLEAN_LOG" \
    && pass "rename moves a file (content preserved)" || fail "rename"
```

Harness verb (writes a fresh source since Task 4's file was removed):
```c
axl_9p_write_file(c, "/ren-src.txt", "hello-9p-write", 14);
if (axl_9p_rename(c, "/ren-src.txt", "/ren-dst.txt") == AXL_OK) {
    AxlBytes *b = NULL;
    if (axl_9p_read_file(c, "/ren-dst.txt", &b) == AXL_OK) {
        axl_printf("RENAME-RB: %.*s\n", (int)axl_bytes_len(b), (const char *)axl_bytes_data(b));
        axl_bytes_unref(b);
    }
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: FAIL (undefined `axl_9p_rename`).

- [ ] **Step 4: Implement**

```c
int
axl_9p_rename(Axl9pClient *c, const char *from, const char *to)
{
    if (c == NULL || from == NULL || to == NULL) {
        return AXL_ERR;
    }
    char parent[512];
    const char *name = NULL;
    if (client_split_parent(to, parent, sizeof(parent), &name) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t fid, dfid;
    if (client_walk(c, from, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    if (client_walk(c, parent, &dfid) != AXL_OK) {
        client_clunk(c, fid);
        return AXL_ERR;
    }
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TRENAME, 0);
    axl_9p_w_u32(&w, fid);           /* fid  (the node being renamed) */
    axl_9p_w_u32(&w, dfid);          /* dfid (new parent directory)   */
    axl_9p_w_str(&w, name);          /* name (new basename)           */
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RRENAME);
    client_clunk(c, dfid);
    client_clunk(c, fid);
    return rc;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `TEST_SKIP_RATCHET=1 timeout 120 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | tail -25`
Expected: PASS "rename moves a file (content preserved)".

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-9p.h src/9p/axl-9p-client.c test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_rename"
```

---

## Task 6: Both-arch validation, docs, review, final commit

**Files:**
- Modify: `src/9p/README.md`
- (verify) `docs/sphinx/modules/` already `.. doxygenfile:: axl-9p.h` — the new functions render automatically.

- [ ] **Step 1: AARCH64 integration + X64 unit ratchet**

Run:
```bash
TEST_SKIP_RATCHET=1 timeout 240 ./test/integration/test-9p-qemu.sh --arch AARCH64 2>&1 | tail -25
TEST_APPS_ONLY=AxlTest9p timeout 120 ./test/integration/test-axl.sh --arch X64 2>&1 | grep -E "Results|Culprit"
TEST_APPS_ONLY=AxlTest9p timeout 150 ./test/integration/test-axl.sh --arch AARCH64 2>&1 | grep -E "Results|Culprit"
```
Expected: all write/mkdir/remove/rename assertions PASS on AARCH64; AxlTest9p GREEN both arches (new codec check counted).

- [ ] **Step 2: Update the module README**

In `src/9p/README.md`, add a "Write path" subsection listing the 4 functions and the wire messages they use, and note the two semantics gotchas the code encodes: `write_file` truncate-or-create (needs an existing parent dir), and `Tremove` clunks server-side (no client clunk). Re-read the existing prose for any "read-only" claim that is now stale and fix it.

- [ ] **Step 3: Doc gates**

Run: `make check-docs check-ascii 2>&1 | grep -iE "clean|error|fail"`
Expected: both "clean".

- [ ] **Step 4: Independent pre-commit review**

Dispatch a fresh reviewer over the Phase 2 diff (per `feedback_code_review_before_commit`, the integration pass). Focus: fid leaks on every early-return in the 4 functions (walk success but op failure must clunk; `Tremove` must NOT double-clunk); `write_file` chunk arithmetic (`msize - 23`, short-write handling, `wbuf` bound); `client_split_parent` edge cases (`"/c"`, trailing slash, over-long parent); Rlerror handling parity with Phase 1. Apply fixes, re-run Step 1.

- [ ] **Step 5: Update the module table + memory**

If `CLAUDE.md`'s Axl9p row still says "Phase 1, no server/write/mount", update it to note the write path landed. Update the Axl9p memory entry.

- [ ] **Step 6: Final commit**

```bash
git add src/9p/README.md CLAUDE.md
git commit -m "9p: docs + module-table update for the Phase 2 write path"
```

---

## Self-Review Notes (author)

- **Spec coverage (Phase 2 row of §12):** `axl_9p_write_file` (Task 3), `mkdir`/`remove`/`rename` (Tasks 4-5), chunked large write (Task 3's Twrite loop). Read chunking already shipped in Phase 1. Mount/server/tool are Phases 3-5, intentionally absent.
- **Infra-first (bucket E):** Task 1 extends `p9-server.py` and re-runs the Phase 1 read tests to prove the infra before any client TDD — the chicken-and-egg the workflow warns about.
- **Verify-before-implementing flags** (real "confirm the exact symbol" checks, not placeholders): Task 1 Step 1 (server helper names), Task 2 Step 4 (`Axl9pWriter` field names in `w_str`), Task 3 Step 5 (`Axl9pReader.error` + `msg_finish` size-patch). Each names the file to grep and adapts 1-2 lines.
- **Type consistency:** `client_split_parent`/`client_lcreate` defined in Task 3 are consumed by name in Tasks 4-5; the 4 public signatures match the spec §6 block and the header exactly. The `0x201` (O_WRONLY|O_TRUNC) and message-type numbers are pinned in Global Constraints.
- **Known interop caveat (documented, not punted):** `write_file` relies on the server honoring `O_TRUNC` in `Tlopen`/`Tlcreate`; our `p9-server.py` and Linux servers do. Real-server interop is exercised in Phase 4.
