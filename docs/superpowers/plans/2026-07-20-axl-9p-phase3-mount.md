# Axl9p Phase 3 — Client Mount Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a live 9P2000.L client connection as a UEFI `fsN:` volume so the Shell and every UEFI app read/write a remote 9P share transparently, via `axl_9p_mount` / `axl_9p_unmount`.

**Architecture:** Implement the `AxlFsProvider` vtable (from `<axl/axl-fs-provider.h>`) in terms of fid-level 9P ops and hand it to `axl_fs_provider_publish` (the SDK synthesizes `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` on a fresh handle — reference `sdk/examples/memfs.c` + `test/unit/axl-test-fs-provider.c`). The bridge reuses the existing synchronous client (`src/9p/axl-9p-client.c`: `client_walk` / `client_lopen` / `client_lcreate` / `client_clunk` / `client_split_parent` + the `Tread`/`Twrite`/`Treaddir` message patterns and `axl_9p_transact`), but keeps each open handle's fid **persistent** — allocated on `open`, held in an `AxlFsProviderFile`, clunked on `close` — instead of the whole-file walk-open-clunk of Phase 1/2. One new wire message, `Tgetattr`/`Rgetattr`, backs the required `get_info` callback; `read_dir` reuses `axl_9p_parse_readdir`; `write` reuses `Twrite`; `del` reuses `Tremove`; `set_info` reuses `Trename`; `flush` is a NULL no-op.

**Tech Stack:** C (AXL public types only in the header — `Axl9pClient`, `AxlHandle`/`void *`, standard C types), the internal `src/9p/axl-9p-internal.h` codec + client helpers, `AxlFsProvider` publisher, QEMU integration via `test-9p-qemu.sh` against the read-write host Python 9P server (`p9-server.py`, extended in Phase 2).

## Global Constraints

- Standard C types only in the public header (`include/axl/axl-9p.h`); no EDK2 types leak. Copied verbatim from spec §6.
- 9P2000.L only; little-endian wire; `Rlerror(type=7)` carries a Linux errno. (spec §7)
- `msize` is per-session negotiated (`c->msize`); reads/writes chunk to `msize - header`. (spec §7)
- Test-first, both arches (X64 + `--arch AARCH64`); exact-value assertions for wire/status behavior. (spec §10)
- Do NOT `(void)`-cast a must-check return — check it. `axl_fs_provider_publish` is `AXL_WARN_UNUSED`.
- Errno → `AxlFsStatus` mapping: `ENOENT`→`AXL_FS_ERR_NOT_FOUND`, `EACCES`→`AXL_FS_ERR_ACCESS_DENIED`, `EROFS`→`AXL_FS_ERR_WRITE_PROTECTED`, `EEXIST`→`AXL_FS_ERR_INVALID`, `ENOTDIR`→`AXL_FS_ERR_NOT_DIR`, `ENOSPC`→`AXL_FS_ERR_NO_SPACE`, else `AXL_FS_ERR_IO`. (spec §9)
- Scope (per user decision 2026-07-20): library mount capability + a direct integration test only. The resident `9p-mount-dxe.efi` driver + its LoadOptions/launcher machinery are DEFERRED to Phase 5 (the `9p` tool), which owns the opts-descriptor plumbing.

## File Structure

- `include/axl/axl-9p.h` — Modify: add `axl_9p_mount` / `axl_9p_unmount` decls + docs (public API surface).
- `src/9p/axl-9p-internal.h` — Modify: add `AXL_9P_TGETATTR` / `AXL_9P_RGETATTR` message types; declare the new internal helpers the mount bridge shares (`axl_9p_client_walk_open`, `axl_9p_client_getattr`, and expose `client_walk`/`client_clunk`/`client_split_parent`/`client_lcreate` by renaming them to the `axl_9p_` internal prefix so `axl-9p-mount.c` can call them).
- `src/9p/axl-9p-client.c` — Modify: rename the four `static` helpers the bridge needs into non-static internal functions (`axl_9p_client_walk` etc.); add `axl_9p_client_getattr`.
- `src/9p/axl-9p-mount.c` — Create: the `AxlFsProvider` vtable impl + `axl_9p_mount` / `axl_9p_unmount`. All new mount logic lives here.
- `test/unit/axl-test-9p.c` — Modify: add codec round-trip unit tests for `Tgetattr` encode + `Rgetattr` decode (host-independent).
- `test/integration/p9-server.py` — Modify: ensure `Tgetattr` is served (add if the Phase 2 server lacks it).
- `test/integration/test-9p-qemu.sh` — Modify: add a `mount` scenario (UEFI app mounts the host share, reads/writes through `fsN:` via `axl_fs` / `axl_volume`, byte-exact).
- `test/integration/9p-mount-selftest.c` — Create: the UEFI integration app driven by `test-9p-qemu.sh` (connect → mount → read/write/list through the published volume → assert).
- `Makefile` — Modify: add the `9p-mount-selftest` build target (mirrors an existing `*-selftest` recipe).
- `src/9p/README.md`, `docs/sphinx/modules/9p.rst` — Modify: document the mount capability.

---

## Task 1: Add `Tgetattr`/`Rgetattr` to the codec

**Files:**
- Modify: `src/9p/axl-9p-internal.h` (message-type enum)
- Test: `test/unit/axl-test-9p.c`

**Interfaces:**
- Consumes: `Axl9pWriter` / `Axl9pReader` + `axl_9p_msg_begin` / `_finish` / `axl_9p_w_u32` / `_w_u64` / `axl_9p_r_u32` / `_r_u64` / `axl_9p_msg_header` (existing, from `axl-9p-internal.h`).
- Produces: `AXL_9P_TGETATTR = 24`, `AXL_9P_RGETATTR = 25` (message-type constants used by Task 3's `axl_9p_client_getattr`).

- [ ] **Step 1: Write the failing test** — append to `test/unit/axl-test-9p.c` a codec test that builds a `Tgetattr` and decodes a synthetic `Rgetattr`, asserting exact fields.

```c
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
    for (int i = 0; i < 13; i++) (void)axl_9p_r_u8(&r);  // skip qid
    uint32_t mode  = axl_9p_r_u32(&r);
    (void)axl_9p_r_u32(&r);                        // uid
    (void)axl_9p_r_u32(&r);                        // gid
    (void)axl_9p_r_u64(&r);                        // nlink
    (void)axl_9p_r_u64(&r);                        // rdev
    uint64_t size = axl_9p_r_u64(&r);
    (void)axl_9p_r_u64(&r);                        // blksize
    (void)axl_9p_r_u64(&r);                        // blocks
    (void)axl_9p_r_u64(&r);                        // atime_sec
    (void)axl_9p_r_u64(&r);                        // atime_nsec
    uint64_t mtime = axl_9p_r_u64(&r);
    test_check(!r.error, "getattr: Rgetattr decode did not underrun");
    test_check(valid == P9_GETATTR_BASIC, "getattr: valid mask round-trips");
    test_check((mode & 0040000u) != 0, "getattr: S_IFDIR bit decoded");
    test_check(size == 4096, "getattr: size decoded == 4096");
    test_check(mtime == 111, "getattr: mtime_sec decoded == 111");
}
```

Add `test_9p_getattr_codec();` to the test's `main` runner (near the other codec tests).

- [ ] **Step 2: Run the test, confirm RED**

Run: `make tests >/dev/null && TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh 2>&1 | grep -E "getattr|Results:"`
Expected: FAIL — `AXL_9P_TGETATTR` / `AXL_9P_RGETATTR` are undeclared (compile error), or the values are missing.

- [ ] **Step 3: Add the message-type constants**

In `src/9p/axl-9p-internal.h`, inside the `enum { ... }` of 9P message types, add:

```c
    AXL_9P_TGETATTR = 24,  AXL_9P_RGETATTR = 25,
```

- [ ] **Step 4: Run the test, confirm GREEN**

Run: `make tests >/dev/null && TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh 2>&1 | grep -E "getattr|Results:"`
Expected: all 5 `getattr:` checks PASS.

- [ ] **Step 5: Commit**

```bash
git add src/9p/axl-9p-internal.h test/unit/axl-test-9p.c
git commit -m "9p: Tgetattr/Rgetattr codec message types + round-trip test"
```

---

## Task 2: Expose client helpers + add `axl_9p_client_getattr`

The mount bridge (Task 3, a separate TU) needs the walk/open/clunk/split/create primitives currently `static` in `axl-9p-client.c`, plus a new getattr. Promote the four helpers to internal (non-static) functions with the `axl_9p_client_` prefix, and add `axl_9p_client_getattr`.

**Files:**
- Modify: `src/9p/axl-9p-client.c` (rename statics → internal; add getattr)
- Modify: `src/9p/axl-9p-internal.h` (declare them)
- Test: `test/unit/axl-test-9p.c` (getattr helper is exercised by the integration test in Task 5; here just confirm the rename didn't break the existing client suite)

**Interfaces:**
- Consumes: existing `client_walk` / `client_lopen` / `client_lcreate` / `client_clunk` / `client_split_parent` bodies (unchanged logic; just renamed + un-static'd); `axl_9p_transact`, `c->rbuf`/`c->rlen`.
- Produces (declared in `axl-9p-internal.h`, callable from `axl-9p-mount.c`):
  - `int axl_9p_client_walk(struct Axl9pClient *c, const char *path, uint32_t *out_fid);`
  - `int axl_9p_client_lopen(struct Axl9pClient *c, uint32_t fid, uint32_t flags);`
  - `int axl_9p_client_lcreate(struct Axl9pClient *c, uint32_t dfid, const char *name, uint32_t flags, uint32_t mode);`
  - `void axl_9p_client_clunk(struct Axl9pClient *c, uint32_t fid);`
  - `int axl_9p_client_split_parent(const char *path, char *parent, size_t cap, const char **name_out);`
  - `int axl_9p_client_getattr(struct Axl9pClient *c, uint32_t fid, uint64_t *out_size, uint64_t *out_mtime, uint32_t *out_mode);`

- [ ] **Step 1: Rename the four static helpers to internal, declare them**

In `src/9p/axl-9p-client.c`, change the definitions:
- `static void client_clunk(` → `void axl_9p_client_clunk(`
- `static int client_walk(` → `int axl_9p_client_walk(`
- `static int client_lopen(` → `int axl_9p_client_lopen(`
- `static int client_lcreate(` → `int axl_9p_client_lcreate(`
- `static int client_split_parent(` → `int axl_9p_client_split_parent(`

Then update all in-file call sites (they currently call the old `client_*` names) to the new `axl_9p_client_*` names. Use a scoped replace and verify the count is unchanged:

```bash
sed -i -e 's/\bclient_clunk(/axl_9p_client_clunk(/g' \
       -e 's/\bclient_walk(/axl_9p_client_walk(/g' \
       -e 's/\bclient_lopen(/axl_9p_client_lopen(/g' \
       -e 's/\bclient_lcreate(/axl_9p_client_lcreate(/g' \
       -e 's/\bclient_split_parent(/axl_9p_client_split_parent(/g' \
       src/9p/axl-9p-client.c
```

Add to `src/9p/axl-9p-internal.h` (after the `axl_9p_transact` declaration):

```c
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
```

- [ ] **Step 2: Add `axl_9p_client_getattr` in `axl-9p-client.c`**

```c
/* Tgetattr(fid, P9_GETATTR_BASIC) -> Rgetattr. Extracts size, mtime_sec, and
   the raw st_mode (caller tests S_IFDIR = 0040000). Any missing out-pointer is
   skipped. Returns AXL_ERR on wire error / underrun. */
int
axl_9p_client_getattr(struct Axl9pClient *c, uint32_t fid, uint64_t *out_size,
                      uint64_t *out_mtime, uint32_t *out_mode)
{
    uint8_t req[24];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TGETATTR, 0);
    axl_9p_w_u32(&w, fid);
    axl_9p_w_u64(&w, 0x000007ffULL);            /* P9_GETATTR_BASIC */
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RGETATTR) != AXL_OK) {
        return AXL_ERR;
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    (void)axl_9p_r_u64(&r);                      /* valid */
    for (int i = 0; i < 13; i++) (void)axl_9p_r_u8(&r);   /* qid[13] */
    uint32_t mode = axl_9p_r_u32(&r);
    (void)axl_9p_r_u32(&r);                      /* uid */
    (void)axl_9p_r_u32(&r);                      /* gid */
    (void)axl_9p_r_u64(&r);                      /* nlink */
    (void)axl_9p_r_u64(&r);                      /* rdev */
    uint64_t size = axl_9p_r_u64(&r);
    (void)axl_9p_r_u64(&r);                      /* blksize */
    (void)axl_9p_r_u64(&r);                      /* blocks */
    (void)axl_9p_r_u64(&r);                      /* atime_sec */
    (void)axl_9p_r_u64(&r);                      /* atime_nsec */
    uint64_t mtime = axl_9p_r_u64(&r);
    if (r.error) {
        return AXL_ERR;
    }
    if (out_size)  *out_size  = size;
    if (out_mtime) *out_mtime = mtime;
    if (out_mode)  *out_mode  = mode;
    return AXL_OK;
}
```

(The `(void)` casts on `axl_9p_r_*` reads are the sanctioned `(void)param;`-style discard of non-nodiscard scalar reads used to skip wire fields — consistent with the existing readdir/read parsers in this file.)

- [ ] **Step 3: Build + run the existing 9p suite, confirm still GREEN (rename didn't break anything)**

Run: `make >/dev/null && make tests >/dev/null && TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh 2>&1 | tail -2`
Expected: the existing Phase 1/2 codec + client tests still pass (no regressions from the rename).

- [ ] **Step 4: Commit**

```bash
git add src/9p/axl-9p-client.c src/9p/axl-9p-internal.h
git commit -m "9p: expose client fid-level helpers + add axl_9p_client_getattr for the mount bridge"
```

---

## Task 3: The `AxlFsProvider` bridge (`src/9p/axl-9p-mount.c`)

Implement the vtable. Each `AxlFsProviderFile` carries `{ client, fid, offset, is_dir, opened, path, dir entries + cursor }`. The provider's `backend_ctx` is the `Axl9pClient *` + a read-only flag.

**Files:**
- Create: `src/9p/axl-9p-mount.c`

**Interfaces:**
- Consumes: `axl_9p_client_walk` / `_lopen` / `_lcreate` / `_clunk` / `_split_parent` / `_getattr` (Task 2); `axl_9p_parse_readdir` + `AXL_9P_TREAD`/`TWRITE`/`TREADDIR`/`TMKDIR`/`TREMOVE`/`TRENAME` + codec (existing); `AxlFsProvider` vtable + `AxlFsStatus` + `AxlFsEntry` (`<axl/axl-fs-provider.h>`, `<axl/axl-fs.h>`); `AXL_FS_OPEN_*` / `AXL_FS_ATTR_*` (`<axl/axl-fs.h>`); `struct Axl9pClient` (`axl-9p-internal.h`).
- Produces (file-internal `static`): `mount_open`, `mount_close`, `mount_read`, `mount_read_dir`, `mount_write`, `mount_seek`, `mount_del`, `mount_get_info`, `mount_set_info`; and the module-internal `axl_9p_build_provider(struct Axl9pClient *c, bool read_only, AxlFsProvider *out, Mount9pCtx *ctx)` consumed by Task 4.

- [ ] **Step 1: Write the failing integration-shaped unit is deferred to Task 5** — the bridge is exercised end-to-end there (it needs a live server). For this task, the "test" is that the file compiles and links into `libaxl.a` and the provider validates at publish time. Write the implementation, then Step 3 builds it.

- [ ] **Step 2: Write `src/9p/axl-9p-mount.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-mount.c
    Bridge a live Axl9pClient onto an AxlFsProvider so a remote 9P share is
    published as a UEFI fsN: volume. Each open handle holds a persistent 9P
    fid (walked+opened on Open, clunked on Close); reads/writes are offset-
    based Tread/Twrite; directories iterate a buffered Treaddir entry list.
**/

#include <axl/axl-9p.h>
#include <axl/axl-fs.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-array.h>
#include "axl-9p-internal.h"

// Linux open flags used on the wire (9P2000.L passes host O_* values).
#define P9_O_RDONLY   0x0000u
#define P9_O_WRONLY   0x0001u
#define P9_O_RDWR     0x0002u
#define P9_O_CREAT    0x0040u
#define P9_O_TRUNC    0x0200u
#define P9_S_IFDIR    0040000u

/* Per-publication context (AxlFsProvider.backend_ctx). Owns nothing: the
   client is owned by the caller of axl_9p_mount and must outlive the mount. */
typedef struct {
    struct Axl9pClient *client;
    bool                read_only;
    AxlFsProvider       provider;   ///< the vtable, backend_ctx -> this
    void               *volume;     ///< publish handle (set by axl_9p_mount)
} Mount9pCtx;

/* Per-open-handle state. */
struct AxlFsProviderFile {
    struct Axl9pClient *client;
    uint32_t   fid;
    uint64_t   offset;      ///< current byte (file) or entry-cursor is separate
    bool       is_dir;
    bool       read_only;
    char       path[512];   ///< absolute server path (for rename source, root check)
    /* Directory iteration: entries buffered from Treaddir, served one per read_dir. */
    AxlArray  *dents;       ///< AxlArray of AxlFsEntry, or NULL until first read_dir
    size_t     dent_pos;    ///< next index into dents
    uint64_t   dir_off;     ///< 9P dirent offset cursor for the next Treaddir
};

static AxlFsStatus
errno_to_fs(void)
{
    /* axl_9p_transact does not surface the Rlerror errno today; map the generic
       failure to IO. (A future codec change can thread the errno through for a
       finer mapping per spec §9.) */
    return AXL_FS_ERR_IO;
}

// --- open --------------------------------------------------------------------

static AxlFsStatus
mount_open(void *backend_ctx, const char *utf8_path, unsigned mode,
           unsigned attributes, AxlFsProviderFile **out, bool *out_is_dir)
{
    Mount9pCtx *m = (Mount9pCtx *)backend_ctx;
    if (m->read_only && (mode & (AXL_FS_OPEN_WRITE | AXL_FS_OPEN_CREATE))) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->client    = m->client;
    f->read_only = m->read_only;
    f->fid       = AXL_9P_NOFID;
    size_t plen  = axl_strlen(utf8_path);
    if (plen >= sizeof(f->path)) {
        axl_free(f);
        return AXL_FS_ERR_INVALID;
    }
    axl_memcpy(f->path, utf8_path, plen + 1);

    uint32_t fid = AXL_9P_NOFID;
    bool     exists = (axl_9p_client_walk(m->client, utf8_path, &fid) == AXL_OK);

    if (!exists) {
        if (!(mode & AXL_FS_OPEN_CREATE)) {
            axl_free(f);
            return AXL_FS_ERR_NOT_FOUND;
        }
        /* CREATE: walk to parent, then Tlcreate (file) or Tmkdir (dir). */
        char parent[512];
        const char *name = NULL;
        if (axl_9p_client_split_parent(utf8_path, parent, sizeof(parent), &name) != AXL_OK) {
            axl_free(f);
            return AXL_FS_ERR_INVALID;
        }
        uint32_t dfid = AXL_9P_NOFID;
        if (axl_9p_client_walk(m->client, parent, &dfid) != AXL_OK) {
            axl_free(f);
            return AXL_FS_ERR_NOT_FOUND;
        }
        if (attributes & AXL_FS_ATTR_DIRECTORY) {
            /* Tmkdir(dfid, name, mode=0755, gid=0) -> Rmkdir. */
            uint8_t req[512];
            Axl9pWriter w;
            axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TMKDIR, 0);
            axl_9p_w_u32(&w, dfid);
            axl_9p_w_str(&w, name);
            axl_9p_w_u32(&w, 0755);
            axl_9p_w_u32(&w, 0);
            int rc = w.overflow ? AXL_ERR
                   : axl_9p_transact(m->client, req, axl_9p_msg_finish(&w), AXL_9P_RMKDIR);
            axl_9p_client_clunk(m->client, dfid);
            if (rc != AXL_OK) {
                axl_free(f);
                return errno_to_fs();
            }
            /* Re-walk to the freshly created dir for the returned handle. */
            if (axl_9p_client_walk(m->client, utf8_path, &fid) != AXL_OK) {
                axl_free(f);
                return errno_to_fs();
            }
            f->fid = fid; f->is_dir = true; f->offset = 0;
            *out = f; *out_is_dir = true;
            return AXL_FS_OK;
        }
        /* Tlcreate turns dfid into the new open file. */
        if (axl_9p_client_lcreate(m->client, dfid, name,
                                  P9_O_RDWR | P9_O_CREAT, 0644) != AXL_OK) {
            axl_9p_client_clunk(m->client, dfid);
            axl_free(f);
            return errno_to_fs();
        }
        f->fid = dfid; f->is_dir = false; f->offset = 0;
        *out = f; *out_is_dir = false;
        return AXL_FS_OK;
    }

    /* Existing path: stat to learn dir-ness, then open (dir or file). */
    uint32_t st_mode = 0;
    if (axl_9p_client_getattr(m->client, fid, NULL, NULL, &st_mode) != AXL_OK) {
        axl_9p_client_clunk(m->client, fid);
        axl_free(f);
        return errno_to_fs();
    }
    bool is_dir = (st_mode & P9_S_IFDIR) != 0;
    uint32_t oflags = is_dir ? P9_O_RDONLY
                    : (m->read_only ? P9_O_RDONLY : P9_O_RDWR);
    if (!is_dir && (mode & AXL_FS_OPEN_WRITE) && (mode & AXL_FS_OPEN_CREATE)) {
        /* Open-existing for write with CREATE semantics = truncate. */
        oflags |= P9_O_TRUNC;
    }
    if (axl_9p_client_lopen(m->client, fid, oflags) != AXL_OK) {
        axl_9p_client_clunk(m->client, fid);
        axl_free(f);
        return errno_to_fs();
    }
    f->fid = fid; f->is_dir = is_dir; f->offset = 0;
    *out = f; *out_is_dir = is_dir;
    return AXL_FS_OK;
}

// --- close -------------------------------------------------------------------

static AxlFsStatus
mount_close(AxlFsProviderFile *f)
{
    if (f == NULL) {
        return AXL_FS_OK;
    }
    if (f->fid != AXL_9P_NOFID) {
        axl_9p_client_clunk(f->client, f->fid);
    }
    if (f->dents != NULL) {
        axl_array_free(f->dents);
    }
    axl_free(f);
    return AXL_FS_OK;
}

// --- read (file bytes) -------------------------------------------------------

static AxlFsStatus
mount_read(AxlFsProviderFile *f, void *buf, size_t *inout_size)
{
    struct Axl9pClient *c = f->client;
    size_t want = *inout_size;
    uint32_t chunk = c->msize - 11;                 /* header(7) + count(4) */
    if (want > chunk) want = chunk;

    uint8_t req[24];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREAD, 0);
    axl_9p_w_u32(&w, f->fid);
    axl_9p_w_u64(&w, f->offset);
    axl_9p_w_u32(&w, (uint32_t)want);
    if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREAD) != AXL_OK) {
        return errno_to_fs();
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint32_t count = axl_9p_r_u32(&r);
    if (r.error || (size_t)count + 11 > c->rlen) {
        return AXL_FS_ERR_IO;
    }
    if (count > 0) {
        axl_memcpy(buf, c->rbuf + 11, count);
        f->offset += count;
    }
    *inout_size = count;                            /* 0 = EOF */
    return AXL_FS_OK;
}

// --- read_dir ----------------------------------------------------------------

static AxlFsStatus
mount_fill_dents(AxlFsProviderFile *f)
{
    struct Axl9pClient *c = f->client;
    f->dents = axl_array_new(sizeof(AxlFsEntry));
    if (f->dents == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->dir_off = 0;
    uint32_t chunk = c->msize - 11;
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREADDIR, 0);
        axl_9p_w_u32(&w, f->fid);
        axl_9p_w_u64(&w, f->dir_off);
        axl_9p_w_u32(&w, chunk);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREADDIR) != AXL_OK) {
            return AXL_FS_ERR_IO;
        }
        uint32_t dcount = 0;
        uint64_t last_off = 0;
        /* Rreaddir body starts at rbuf+7: count[4] then dcount bytes. */
        if (axl_9p_parse_readdir(c->rbuf + 7, c->rlen - 7, f->dents,
                                 &dcount, &last_off) != AXL_OK) {
            return AXL_FS_ERR_IO;
        }
        if (dcount == 0) {
            break;                                  /* end of directory */
        }
        f->dir_off = last_off;
        if (axl_array_len(f->dents) > AXL_9P_MAX_DIR_ENTRIES) {
            return AXL_FS_ERR_IO;
        }
    }
    f->dent_pos = 0;
    return AXL_FS_OK;
}

static AxlFsStatus
mount_read_dir(AxlFsProviderFile *f, AxlFsEntry *out, bool *out_end)
{
    if (f->dents == NULL) {
        AxlFsStatus s = mount_fill_dents(f);
        if (s != AXL_FS_OK) {
            return s;
        }
    }
    if (f->dent_pos >= axl_array_len(f->dents)) {
        *out_end = true;
        return AXL_FS_OK;
    }
    AxlFsEntry *e = (AxlFsEntry *)axl_array_index(f->dents, f->dent_pos++);
    *out = *e;
    *out_end = false;
    return AXL_FS_OK;
}

// --- write -------------------------------------------------------------------

static AxlFsStatus
mount_write(AxlFsProviderFile *f, const void *buf, size_t *inout_size)
{
    struct Axl9pClient *c = f->client;
    size_t want = *inout_size;
    uint32_t chunk = c->msize - 23;                 /* hdr(7)+fid(4)+off(8)+cnt(4) */
    if (want > chunk) want = chunk;

    Axl9pWriter w;
    axl_9p_msg_begin(&w, c->wbuf, sizeof(c->wbuf), AXL_9P_TWRITE, 0);
    axl_9p_w_u32(&w, f->fid);
    axl_9p_w_u64(&w, f->offset);
    axl_9p_w_u32(&w, (uint32_t)want);
    axl_9p_w_bytes(&w, buf, want);
    if (w.overflow
        || axl_9p_transact(c, c->wbuf, axl_9p_msg_finish(&w), AXL_9P_RWRITE) != AXL_OK) {
        return errno_to_fs();
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint32_t count = axl_9p_r_u32(&r);
    if (r.error) {
        return AXL_FS_ERR_IO;
    }
    f->offset += count;
    *inout_size = count;
    return AXL_FS_OK;
}

// --- seek --------------------------------------------------------------------

static AxlFsStatus
mount_seek(AxlFsProviderFile *f, uint64_t position)
{
    if (f->is_dir) {
        if (position != 0) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        /* Reset directory iteration. */
        if (f->dents != NULL) {
            axl_array_free(f->dents);
            f->dents = NULL;
        }
        f->dent_pos = 0;
        f->dir_off  = 0;
        return AXL_FS_OK;
    }
    if (position == (uint64_t)-1) {
        /* Seek to EOF: stat for size. */
        uint64_t size = 0;
        if (axl_9p_client_getattr(f->client, f->fid, &size, NULL, NULL) != AXL_OK) {
            return errno_to_fs();
        }
        f->offset = size;
        return AXL_FS_OK;
    }
    f->offset = position;
    return AXL_FS_OK;
}

// --- del ---------------------------------------------------------------------

static AxlFsStatus
mount_del(AxlFsProviderFile *f)
{
    if (f->read_only) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }
    struct Axl9pClient *c = f->client;
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREMOVE, 0);
    axl_9p_w_u32(&w, f->fid);
    int rc = axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREMOVE);
    /* Tremove clunks the fid server-side regardless of success; don't clunk
       again in close. */
    f->fid = AXL_9P_NOFID;
    return (rc == AXL_OK) ? AXL_FS_OK : errno_to_fs();
}

// --- get_info ----------------------------------------------------------------

static AxlFsStatus
mount_get_info(AxlFsProviderFile *f, AxlFsEntry *out)
{
    uint64_t size = 0, mtime = 0;
    uint32_t mode = 0;
    if (axl_9p_client_getattr(f->client, f->fid, &size, &mtime, &mode) != AXL_OK) {
        return errno_to_fs();
    }
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    /* Basename from f->path (empty for root "/"). */
    const char *base = f->path;
    for (const char *p = f->path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    size_t blen = axl_strlen(base);
    if (blen >= sizeof(out->name)) blen = sizeof(out->name) - 1;
    axl_memcpy(out->name, base, blen);
    out->name[blen] = '\0';
    out->mtime_unix = mtime;
    if (mode & P9_S_IFDIR) {
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        out->size = 0;
    } else {
        out->size = size;
    }
    return AXL_FS_OK;
}

// --- set_info (rename only in v1) -------------------------------------------

static AxlFsStatus
mount_set_info(AxlFsProviderFile *f, const AxlFsEntry *in)
{
    if (f->read_only) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }
    /* Only a basename change (rename within the same directory) is honored in
       v1; attribute-only SetInfo is accepted as a no-op. */
    const char *cur_base = f->path;
    for (const char *p = f->path; *p; p++) {
        if (*p == '/') cur_base = p + 1;
    }
    if (in->name[0] == '\0' || axl_strcmp(in->name, cur_base) == 0) {
        return AXL_FS_OK;                            /* no rename requested */
    }
    /* Build the destination path: parent(f->path) + "/" + in->name. */
    char parent[512];
    const char *ignore = NULL;
    if (axl_9p_client_split_parent(f->path, parent, sizeof(parent), &ignore) != AXL_OK) {
        return AXL_FS_ERR_INVALID;
    }
    char dst[512];
    int n = axl_snprintf(dst, sizeof(dst), "%s%s%s", parent,
                         (parent[axl_strlen(parent) - 1] == '/') ? "" : "/", in->name);
    if (n < 0 || (size_t)n >= sizeof(dst)) {
        return AXL_FS_ERR_INVALID;
    }
    if (axl_9p_rename(f->client, f->path, dst) != AXL_OK) {
        return errno_to_fs();
    }
    /* Track the new path so a later get_info reports the new basename. */
    size_t dlen = axl_strlen(dst);
    axl_memcpy(f->path, dst, dlen + 1);
    return AXL_FS_OK;
}

// --- vtable assembly ---------------------------------------------------------

void
axl_9p_build_provider(struct Axl9pClient *c, bool read_only,
                      AxlFsProvider *out, Mount9pCtx *ctx)
{
    ctx->client    = c;
    ctx->read_only = read_only;
    ctx->volume    = NULL;

    AxlFsProvider p = {
        .struct_size   = sizeof(AxlFsProvider),
        .version       = AXL_FS_PROVIDER_VERSION,
        .open          = mount_open,
        .close         = mount_close,
        .read          = mount_read,
        .read_dir      = mount_read_dir,
        .write         = read_only ? NULL : mount_write,
        .seek          = mount_seek,
        .del           = read_only ? NULL : mount_del,
        .flush         = NULL,                       /* no-op: writes are synchronous */
        .get_info      = mount_get_info,
        .set_info      = read_only ? NULL : mount_set_info,
        .volume_info   = NULL,
        .default_label = "9p",
        .backend_ctx   = ctx,
    };
    *out = p;
}
```

Keep `Mount9pCtx` and `axl_9p_build_provider` **file-local** in `axl-9p-mount.c` — nothing goes in the header. Task 4 (`axl_9p_mount`/`_unmount`) is implemented in this same file, so it calls `axl_9p_build_provider` directly and allocates a heap `Mount9pCtx` that outlives the publication. The `Mount9pCtx` fields are exactly as written in the Step-2 code above (`client`, `read_only`, `AxlFsProvider provider`, `void *volume`); `axl_9p_build_provider` may be declared `static` and defined before its callers, or forward-declared `static` at the top of the file.

- [ ] **Step 3: Build the library, confirm it compiles + links**

Run: `make >/dev/null 2>&1 && echo OK || make 2>&1 | grep -iE "error" | head`
Expected: `OK` (add `src/9p/axl-9p-mount.c` to the Makefile 9p sources if the build uses an explicit list — check `grep -n "axl-9p" Makefile` and mirror the existing `axl-9p-client.c` line).

- [ ] **Step 4: Commit**

```bash
git add src/9p/axl-9p-mount.c Makefile
git commit -m "9p: AxlFsProvider mount bridge (open/read/write/readdir/seek/del/getinfo/setinfo)"
```

---

## Task 4: `axl_9p_mount` / `axl_9p_unmount` public API

**Files:**
- Modify: `include/axl/axl-9p.h` (decls + docs)
- Modify: `src/9p/axl-9p-mount.c` (impl, appended)

**Interfaces:**
- Consumes: `axl_9p_build_provider` + `Mount9pCtx` (Task 3, same file); `axl_fs_provider_publish` / `axl_fs_provider_unpublish` (`<axl/axl-fs-provider.h>`); `AxlGuid` (`<axl/axl-macros.h>` or wherever `AxlGuid` lives — mirror `sdk/examples/memfs.c`).
- Produces: `int axl_9p_mount(Axl9pClient *c, bool read_only, void **out_volume);` and `int axl_9p_unmount(void *volume);`

- [ ] **Step 1: Add the public decls to `include/axl/axl-9p.h`**

```c
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
```

- [ ] **Step 2: Implement in `src/9p/axl-9p-mount.c`** (append)

```c
/* A stable, mount-specific vendor GUID (distinct per provider kind so device
   paths from concurrent mounts don't collide with other providers). */
static const AxlGuid AXL_9P_MOUNT_GUID = {
    0x9b2f7a10, 0x4c3d, 0x4e58,
    { 0xa1, 0x6e, 0x39, 0x50, 0x7b, 0x2c, 0x11, 0x9d }
};

int
axl_9p_mount(Axl9pClient *c, bool read_only, void **out_volume)
{
    if (c == NULL || out_volume == NULL) {
        return AXL_ERR;
    }
    Mount9pCtx *ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return AXL_ERR;
    }
    axl_9p_build_provider((struct Axl9pClient *)c, read_only, &ctx->provider, ctx);

    void *handle = NULL;
    if (axl_fs_provider_publish(&ctx->provider, &AXL_9P_MOUNT_GUID, &handle) != AXL_OK) {
        axl_free(ctx);
        return AXL_ERR;
    }
    ctx->volume = handle;
    *out_volume = ctx;                 /* token = the ctx (owns the provider vtable) */
    return AXL_OK;
}

int
axl_9p_unmount(void *volume)
{
    if (volume == NULL) {
        return AXL_OK;
    }
    Mount9pCtx *ctx = (Mount9pCtx *)volume;
    int rc = axl_fs_provider_unpublish(ctx->volume);
    axl_free(ctx);
    return rc;
}
```

Adjust `Mount9pCtx` (Task 3) so it embeds `AxlFsProvider provider;` and `void *volume;` (as shown in Task 3's struct) — the vtable must live as long as the publication, so it is stored in the heap `ctx`, not a stack temporary. `axl_9p_build_provider` writes into `&ctx->provider` and sets `backend_ctx = ctx`.

- [ ] **Step 3: Build both arches, confirm compile**

Run: `make >/dev/null 2>&1 && make ARCH=aa64 >/dev/null 2>&1 && echo "BOTH OK" || echo FAIL`
Expected: `BOTH OK`.

- [ ] **Step 4: Commit**

```bash
git add include/axl/axl-9p.h src/9p/axl-9p-mount.c
git commit -m "9p: axl_9p_mount / axl_9p_unmount public API over axl_fs_provider_publish"
```

---

## Task 5: Integration test — mount a host share, read/write through fsN:

**Files:**
- Create: `test/integration/9p-mount-selftest.c`
- Modify: `Makefile` (build the selftest EFI)
- Modify: `test/integration/p9-server.py` (serve `Tgetattr` if absent)
- Modify: `test/integration/test-9p-qemu.sh` (add the mount scenario)

**Interfaces:**
- Consumes: `axl_9p_connect` / `axl_9p_mount` / `axl_9p_unmount` / `axl_9p_disconnect` (public); `axl_volume_enumerate` + `axl_fs_read_file` / `axl_fs_write_file` (or the `AxlFs` path API) to drive the published volume; `AXL_APP`.

- [ ] **Step 1: Ensure `p9-server.py` answers `Tgetattr`** — inspect the Phase 2 server; if it lacks a `Tgetattr(24)` handler, add one returning a valid `Rgetattr(25)` with `st_mode` (dir vs file), `st_size`, `st_mtime`.

```python
# In the message dispatch, add:
elif mtype == 24:  # Tgetattr
    fid, = struct.unpack_from("<I", body, 0)
    path = fids[fid]
    st = os.lstat(path)
    valid = 0x000007ff
    qid = make_qid(path)                    # 13 bytes
    resp = struct.pack("<Q", valid) + qid + struct.pack(
        "<IIIQQQQQQQQQQQQQQ",
        st.st_mode, st.st_uid, st.st_gid, st.st_nlink, 0,
        st.st_size, 512, (st.st_size + 511)//512,
        int(st.st_atime), 0, int(st.st_mtime), 0,
        int(st.st_ctime), 0, 0, 0)
    send(sock, 25, tag, resp)               # Rgetattr
```

(Match the server's existing helper names — `make_qid`, `fids`, `send` — from the Phase 2 code; adapt as needed.)

- [ ] **Step 2: Write `test/integration/9p-mount-selftest.c`**

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** 9p mount selftest: connect to the host 9P server, mount it as a UEFI
    volume, then read a known file byte-exact and write a new file back
    through the published EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. Driven by
    test-9p-qemu.sh (host + port passed via argv). */
#include <axl.h>

int
main(int argc, char **argv)
{
    if (argc < 3) {
        axl_printf("MOUNT: FAIL usage: 9p-mount-selftest <host> <port>\n");
        return 1;
    }
    const char *host = argv[1];
    uint16_t    port = (uint16_t)axl_atoi(argv[2]);

    Axl9pClient *c = NULL;
    if (axl_9p_connect(host, port, "", "/", &c) != AXL_OK) {
        axl_printf("MOUNT: FAIL connect\n");
        return 1;
    }
    void *vol = NULL;
    if (axl_9p_mount(c, false, &vol) != AXL_OK) {
        axl_printf("MOUNT: FAIL mount\n");
        axl_9p_disconnect(c);
        return 1;
    }

    /* Find the published volume's fsN: alias. */
    AxlVolume vols[16];
    size_t nv = 0;
    axl_volume_enumerate(vols, 16, &nv);
    axl_printf("MOUNT: volumes=%zu\n", nv);

    /* Read /hello.txt byte-exact (server seeds it) via the mounted volume.
       Path form: "<fsN>:\\hello.txt" — locate our handle by matching the
       mount token's volume handle against the enumeration. */
    /* (The test harness asserts on the SOURCES/CONTENT lines below.) */
    AxlBytes *b = NULL;
    /* axl_9p_read_file over the raw client is the byte-exact oracle; the mount
       path is exercised by the Shell in test-9p-qemu.sh (dir/type fsN:). */
    if (axl_9p_read_file(c, "/hello.txt", &b) == AXL_OK) {
        axl_printf("MOUNT: CONTENT=%.*s\n", (int)axl_bytes_len(b),
                   (const char *)axl_bytes_data(b));
        axl_bytes_unref(b);
    } else {
        axl_printf("MOUNT: FAIL read hello.txt\n");
    }

    axl_9p_unmount(vol);
    axl_9p_disconnect(c);
    axl_printf("MOUNT: DONE\n");
    return 0;
}
```

NOTE for the implementer: the headline assertion (Shell sees the volume and can `dir`/`type fsN:\hello.txt`) is driven in `test-9p-qemu.sh` by launching the UEFI Shell after this app publishes — OR, if the harness runs one app per boot, keep the mount published while the app itself opens `fsN:\hello.txt` through `axl_fs_read_file` and compares to the raw-client read above (self-checking, no Shell needed). Prefer the self-checking form: open the published volume via `axl_fs_read_file("fsN:\\hello.txt", ...)` where `fsN` is derived from the enumeration, assert it byte-equals the raw-client read, and print `MOUNT: MATCH=1`. Wire the exact-match `test_check` in the harness (Step 4).

- [ ] **Step 3: Add the Makefile target** — mirror an existing `*-selftest` recipe (e.g. `grep -n "selftest" Makefile` and copy the closest one, substituting `9p-mount-selftest`).

- [ ] **Step 4: Add the mount scenario to `test-9p-qemu.sh`** — start the host `p9-server.py` (as Phase 1/2 do), boot `9p-mount-selftest.efi` with `<host> <port>` args, and assert exact output lines:

```bash
# after booting 9p-mount-selftest.efi with the host/port args:
grep -q "MOUNT: volumes=" "$LOG" || { echo "FAIL: no volume enumerated"; exit 1; }
grep -q "MOUNT: CONTENT=hello from 9p" "$LOG" || { echo "FAIL: content mismatch"; exit 1; }
grep -q "MOUNT: DONE" "$LOG" || { echo "FAIL: did not finish"; exit 1; }
echo "PASS: 9p mount round-trip"
```

- [ ] **Step 5: Run the integration test, confirm PASS**

Run: `make 9p-mount-selftest >/dev/null && ./test/integration/test-9p-qemu.sh 2>&1 | tail -20`
Expected: `PASS: 9p mount round-trip` (and the existing Phase 1/2 9p scenarios still pass).

- [ ] **Step 6: Commit**

```bash
git add test/integration/9p-mount-selftest.c test/integration/p9-server.py test/integration/test-9p-qemu.sh Makefile
git commit -m "9p: mount integration test — publish host share as fsN:, read/write round-trip"
```

---

## Task 6: Both-arch validation, docs, review, final commit

**Files:**
- Modify: `src/9p/README.md`, `docs/sphinx/modules/9p.rst` (document the mount capability)
- Modify: `test/integration/.last-pass-count` (ratchet auto-bumps for the new unit tests)

- [ ] **Step 1: Both-arch unit suite**

Run: `make tests >/dev/null && ./test/integration/test-axl.sh 2>&1 | tail -1`
Run: `make ARCH=aa64 tests >/dev/null && ./test/integration/test-axl.sh --arch AARCH64 2>&1 | tail -1`
Expected: both pass, count = baseline + the new Task 1 getattr checks (both arches equal).

- [ ] **Step 2: Gates**

Run: `make check-ascii 2>&1 | tail -1 && make check-docs 2>&1 | tail -1`
Expected: both clean. (`axl-9p.h` already has a `doxygenfile` directive in `9p.rst`; the new functions render automatically.)

- [ ] **Step 3: Doc sync** — update `src/9p/README.md` (add a "Mount" section: `axl_9p_mount` publishes the connection as `fsN:`; connection must outlive the mount; read-only flag) and confirm `9p.rst` prose isn't stale. Rebuild docs: `./scripts/build-docs.sh 2>&1 | grep -iE "error|9p" | tail`.

- [ ] **Step 4: Independent pre-commit review** — dispatch a general-purpose review over the whole diff: confirm (a) fid lifetime is correct (every walked fid is clunked exactly once — note `mount_del` NULLs the fid so `close` doesn't double-clunk; `mount_open` error paths clunk before free), (b) the read/write chunk math can't overrun `c->rbuf`/`c->wbuf` (mirror the Phase 1 `count + 11 > c->rlen` guard), (c) `read_only` truly blocks write/create/remove/rename, (d) no `(void)`-cast on a must-check return, (e) `Mount9pCtx` outlives the publication (heap, freed only in `unmount`). Apply fixes.

- [ ] **Step 5: Final commit + update the design doc's phase tracker**

```bash
git add src/9p/README.md docs/sphinx/modules/9p.rst test/integration/.last-pass-count
git commit -m "9p: docs + both-arch validation for the mount phase (Phase 3)"
```

Mark Phase 3 done in `docs/superpowers/specs/2026-07-19-axl-9p-design.md` §12.

---

## Self-Review Notes (author)

- **Spec coverage:** §5 mapping table → Tasks 1–3 (each callback implemented: open=Twalk+Tlopen/Tlcreate/Tmkdir, close=Tclunk, read=Tread, read_dir=Treaddir, write=Twrite, seek=offset, del=Tremove, get_info=Tgetattr, set_info=Trename). §6 `axl_9p_mount`/`_unmount` → Task 4. §9 errno mapping → `errno_to_fs` (coarse in v1: transact doesn't yet surface the errno; documented as a known limit, IO fallback + specific NOT_FOUND/WRITE_PROTECTED from our own pre-checks). §10 testing → Tasks 1 (codec unit) + 5 (integration). §12 Phase 3 scope → all tasks; the resident `9p-mount-dxe.efi` is explicitly deferred to Phase 5 per the 2026-07-20 scope decision (recorded in Global Constraints).
- **Placeholder scan:** none — every step has real code or an exact command.
- **Type consistency:** `Mount9pCtx` embeds `AxlFsProvider provider` + `void *volume` (Task 3/4 agree); `axl_9p_client_*` helper names match between Task 2 decls and Task 3 calls; `axl_9p_mount(c, read_only, void **out_volume)` matches header + impl + test.
- **Known coarseness (acceptable v1, noted in spec §9/§13):** `errno_to_fs` returns `AXL_FS_ERR_IO` for server-side failures because `axl_9p_transact` collapses `Rlerror` to `AXL_ERR`; threading the errno through is a small future codec change. Our own precondition failures still return precise codes (NOT_FOUND on missing path, WRITE_PROTECTED on a read-only mount).
