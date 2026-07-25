# Axl9p Phase 1 — Codec + Client Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the 9P2000.L wire codec and a read-only client core (`axl_9p_connect`, `axl_9p_read_file`, `axl_9p_list`) as a new SDK module, proven by codec unit tests and a QEMU client integration test against a host 9P server.

**Architecture:** A new `Axl9p` module (`src/9p/`, public `<axl/axl-9p.h>`). An internal codec (`Axl9pWriter`/`Axl9pReader` little-endian cursors + message framing) is shared by client and (future) server. The Phase-1 client is synchronous over the existing `AxlTcp` blocking API: connect → `Tversion`/`Tattach`, then per-op `Twalk`/`Tlopen`/`Tread`/`Treaddir`/`Tclunk` sequences. No server, no write path, no mount — those are later phases.

**Tech Stack:** C (gnu2x, freestanding UEFI), AXL SDK internals (`AxlTcp` sync client, `AxlBytes`, `AxlArray`, `AxlFsEntry`), the AXL test harness (QEMU unit binaries + integration shell scripts), Python 3 for the host test server.

## Global Constraints

- **Coding style** (`docs/AXL-Coding-Style.md`): `axl_snake_case` functions, `AxlPascalCase` types, `AXL_SCREAMING_CASE` macros. 4-space indent, K&R braces, no space before parens. Multi-line function signatures even for one param. Public API uses **standard C types only** — never UEFI types leak through `<axl/axl-9p.h>`.
- **Doc comments**: `///<` inline for params/fields; `@brief`/`@return` in block comments on public declarations.
- **No non-ASCII** in string/char literals (`make check-ascii` gate).
- **Test-first** (CLAUDE.md): write the failing test, confirm RED, implement to GREEN, refactor while green, then review. Codec output/format assertions use **exact** comparisons (`axl_memcmp` of golden bytes / `axl_strcmp`), never substring.
- **Both-arch validation**: every phase ends GREEN on `ARCH=x64` and `ARCH=aa64`.
- **Ratchet**: `test/integration/.last-pass-count` is the unit baseline; it rises as unit tests are added. Balance SKIP-path vs populated-path check counts if any topology gating is introduced (none expected in Phase 1).
- **Protocol**: 9P2000.L only. Little-endian wire encoding throughout. Header is `size[4] type[1] tag[2]`; `string = len[2] + utf8`; `qid = type[1] version[4] path[8]` (13 bytes). `Rlerror` type is 7 and carries `ecode[4]` (Linux errno).
- **Message type constants** (used across tasks):
  `Tversion=100 Rversion=101 Rlerror=7 Tattach=104 Rattach=105 Twalk=110 Rwalk=111 Tlopen=12 Rlopen=13 Tread=116 Rread=117 Treaddir=40 Rreaddir=41 Tclunk=120 Rclunk=121`.
  `NOFID=0xFFFFFFFF`, `NONUNAME=0xFFFFFFFF`, default `msize=8192`.
- **Reference (do not copy; TDD from scratch)**: the throwaway spike proved the wire format — `test/unit/axl-test-net.c` `run_9p_spike_mode`, `test/integration/p9-spike-server.py`, `test/integration/test-9p-spike-qemu.sh`. These are **deleted in Task 8**.

---

## File Structure

- `include/axl/axl-9p.h` — public API (Phase 1 subset: `Axl9pClient`, `axl_9p_connect`, `axl_9p_disconnect`, `axl_9p_read_file`, `axl_9p_list`).
- `src/9p/axl-9p-internal.h` — internal: message-type constants, `Axl9pWriter`/`Axl9pReader`, framing decls, `Axl9pClient` struct.
- `src/9p/axl-9p-codec.c` — writer/reader primitives + `msg_begin`/`msg_finish` + header parse.
- `src/9p/axl-9p-client.c` — connect / read_file / list, and the transact helper over `AxlTcp`.
- `test/unit/axl-test-9p.c` — new `AxlTest9p` binary: codec round-trip + golden-bytes unit tests.
- `test/integration/p9-server.py` — host read+list 9P2000.L server (fixed tree).
- `test/integration/test-9p-qemu.sh` — client integration (QEMU guest → host server).
- `Makefile` — build `src/9p/*.c` into libaxl; add `AxlTest9p`; install `axl-9p.h`.
- `test/integration/test-axl.sh` — add `AxlTest9p` to `TEST_APPS`.
- `test/unit/axl-test-net.c` — add a `9p-client <host> <port>` mode that drives the real API for the integration test.
- `src/9p/README.md`, `docs/sphinx/modules/net.rst` (or a new page), `CLAUDE.md` module table — docs.

---

## Task 1: Module scaffold + codec primitives (writer/reader round-trip)

Infra-first (new module + test binary must build and run), then TDD the primitives.

**Files:**
- Create: `src/9p/axl-9p-internal.h`, `src/9p/axl-9p-codec.c`, `test/unit/axl-test-9p.c`
- Modify: `Makefile` (add `src/9p` to lib sources; add `BUILD_TEST` for `AxlTest9p`), `test/integration/test-axl.sh:32` (add `AxlTest9p` to `TEST_APPS`)

**Interfaces:**
- Produces: `Axl9pWriter`, `Axl9pReader`, `axl_9p_w_u8/u16/u32/u64/str`, `axl_9p_r_u8/u16/u32/u64`, `axl_9p_r_str(reader, char *out, size_t cap)`. All little-endian. Writer tracks `overflow`; reader tracks `error` (set on short read).

- [ ] **Step 1: Create the internal header with the cursor types and primitive decls**

Create `src/9p/axl-9p-internal.h`:

```c
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
};

#define AXL_9P_NOFID      0xFFFFFFFFu
#define AXL_9P_NONUNAME   0xFFFFFFFFu
#define AXL_9P_MSIZE      8192u
#define AXL_9P_QID_LEN    13u

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
void   axl_9p_w_u8 (Axl9pWriter *w, uint8_t v);
void   axl_9p_w_u16(Axl9pWriter *w, uint16_t v);
void   axl_9p_w_u32(Axl9pWriter *w, uint32_t v);
void   axl_9p_w_u64(Axl9pWriter *w, uint64_t v);
void   axl_9p_w_str(Axl9pWriter *w, const char *s);

void     axl_9p_r_init(Axl9pReader *r, const uint8_t *buf, size_t len);
uint8_t  axl_9p_r_u8 (Axl9pReader *r);
uint16_t axl_9p_r_u16(Axl9pReader *r);
uint32_t axl_9p_r_u32(Axl9pReader *r);
uint64_t axl_9p_r_u64(Axl9pReader *r);
/* Copy a 9P string into out[cap] (NUL-terminated, truncated to cap-1).
   Returns the on-wire string length. */
size_t   axl_9p_r_str(Axl9pReader *r, char *out, size_t cap);

#endif /* AXL_9P_INTERNAL_H */
```

- [ ] **Step 2: Write the failing codec round-trip test**

Create `test/unit/axl-test-9p.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

#include <axl.h>
#include "axl-test.h"                 /* test_check / TEST_MAIN harness */
#include "../../src/9p/axl-9p-internal.h"

static void
test_codec_roundtrip(void)
{
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_w_init(&w, buf, sizeof(buf));
    axl_9p_w_u8(&w, 0x12);
    axl_9p_w_u16(&w, 0x3456);
    axl_9p_w_u32(&w, 0x89abcdefu);
    axl_9p_w_u64(&w, 0x0011223344556677ull);
    axl_9p_w_str(&w, "hello");
    test_check(!w.overflow, "9p codec: writer did not overflow");

    Axl9pReader r;
    axl_9p_r_init(&r, buf, w.len);
    test_check(axl_9p_r_u8(&r)  == 0x12,             "9p codec: u8 roundtrip");
    test_check(axl_9p_r_u16(&r) == 0x3456,           "9p codec: u16 roundtrip");
    test_check(axl_9p_r_u32(&r) == 0x89abcdefu,      "9p codec: u32 roundtrip");
    test_check(axl_9p_r_u64(&r) == 0x0011223344556677ull, "9p codec: u64 roundtrip");
    char s[16];
    size_t n = axl_9p_r_str(&r, s, sizeof(s));
    test_check(n == 5 && axl_strcmp(s, "hello") == 0, "9p codec: str roundtrip");
    test_check(!r.error, "9p codec: reader did not underrun");
}

static void
test_codec_overflow_and_underrun(void)
{
    uint8_t small[2];
    Axl9pWriter w;
    axl_9p_w_init(&w, small, sizeof(small));
    axl_9p_w_u32(&w, 0xdeadbeefu);          /* 4 bytes into a 2-byte buffer */
    test_check(w.overflow, "9p codec: overflow flagged on short buffer");

    uint8_t one[1] = { 0xAB };
    Axl9pReader r;
    axl_9p_r_init(&r, one, sizeof(one));
    (void)axl_9p_r_u32(&r);                 /* reads past end */
    test_check(r.error, "9p codec: underrun flagged on short read");
}

TEST_MAIN(
    test_codec_roundtrip,
    test_codec_overflow_and_underrun
)
```

> Note: confirm the exact test-harness entry macro by reading an existing small
> binary (`test/unit/axl-test-math.c` or similar) — match its `#include`s and
> its `main`/`TEST_MAIN`/registration pattern exactly. Adjust the two lines above
> (`#include "axl-test.h"` and the `TEST_MAIN(...)` footer) to that pattern.

- [ ] **Step 3: Wire the build (module sources + test binary + runner)**

In `Makefile`: add `src/9p/axl-9p-codec.c` and `src/9p/axl-9p-client.c` to the library source list (follow how `src/net/*.c` are listed), and register the test binary next to `AxlTestNet`:

```make
$(eval $(call BUILD_TEST,AxlTest9p,axl-test-9p))
```

Also add `AxlTest9p` to the unit-test name list near line 1991 (the `BUILD_TEST` roster) so `make tests` builds it.

In `test/integration/test-axl.sh:32`, append `AxlTest9p` to the `TEST_APPS=(...)` array.

- [ ] **Step 4: Run the test to verify it FAILS (link error: codec symbols undefined)**

Run: `make ARCH=x64 tests 2>&1 | grep -iE 'axl_9p_w_init|undefined|AxlTest9p'`
Expected: FAIL — `undefined reference to 'axl_9p_w_init'` (and siblings).

- [ ] **Step 5: Implement the codec primitives**

Create `src/9p/axl-9p-codec.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-codec.c
    9P2000.L little-endian wire codec: write/read cursors + message framing.
**/

#include "axl-9p-internal.h"
#include <axl/axl-str.h>     /* axl_strlen, axl_memcpy */

void
axl_9p_w_init(Axl9pWriter *w, uint8_t *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false;
}

static void
w_bytes(Axl9pWriter *w, const uint8_t *p, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        w->buf[w->len + i] = p[i];
    }
    w->len += n;
}

void axl_9p_w_u8(Axl9pWriter *w, uint8_t v)  { w_bytes(w, &v, 1); }

void
axl_9p_w_u16(Axl9pWriter *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    w_bytes(w, b, 2);
}

void
axl_9p_w_u32(Axl9pWriter *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    w_bytes(w, b, 4);
}

void
axl_9p_w_u64(Axl9pWriter *w, uint64_t v)
{
    axl_9p_w_u32(w, (uint32_t)v);
    axl_9p_w_u32(w, (uint32_t)(v >> 32));
}

void
axl_9p_w_str(Axl9pWriter *w, const char *s)
{
    size_t n = axl_strlen(s);
    axl_9p_w_u16(w, (uint16_t)n);
    w_bytes(w, (const uint8_t *)s, n);
}

void
axl_9p_r_init(Axl9pReader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf; r->len = len; r->pos = 0; r->error = false;
}

static bool
r_need(Axl9pReader *r, size_t n)
{
    if (r->pos + n > r->len) {
        r->error = true;
        return false;
    }
    return true;
}

uint8_t
axl_9p_r_u8(Axl9pReader *r)
{
    if (!r_need(r, 1)) return 0;
    return r->buf[r->pos++];
}

uint16_t
axl_9p_r_u16(Axl9pReader *r)
{
    if (!r_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->buf[r->pos] | (r->buf[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

uint32_t
axl_9p_r_u32(Axl9pReader *r)
{
    if (!r_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->buf[r->pos]
               | ((uint32_t)r->buf[r->pos + 1] << 8)
               | ((uint32_t)r->buf[r->pos + 2] << 16)
               | ((uint32_t)r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

uint64_t
axl_9p_r_u64(Axl9pReader *r)
{
    uint64_t lo = axl_9p_r_u32(r);
    uint64_t hi = axl_9p_r_u32(r);
    return lo | (hi << 32);
}

size_t
axl_9p_r_str(Axl9pReader *r, char *out, size_t cap)
{
    uint16_t n = axl_9p_r_u16(r);
    if (!r_need(r, n)) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    size_t copy = (n < cap - 1) ? n : (cap - 1);
    axl_memcpy(out, r->buf + r->pos, copy);
    out[copy] = '\0';
    r->pos += n;
    return n;
}
```

- [ ] **Step 6: Run the codec test to verify it PASSES**

Run: `TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh --arch X64 2>&1 | grep -iE '9p codec|Results:'`
Expected: all `9p codec:` checks PASS; a `Results: N passed, 0 failed` footer.

- [ ] **Step 7: Refactor while green** — re-read the codec for duplication and naming; confirm the test still passes. Commit.

```bash
git add src/9p/axl-9p-internal.h src/9p/axl-9p-codec.c test/unit/axl-test-9p.c \
        Makefile test/integration/test-axl.sh
git commit -m "9p: codec write/read cursors (little-endian primitives) + AxlTest9p"
```

---

## Task 2: Message framing (`msg_begin`/`msg_finish`) + golden-bytes Tversion

**Files:**
- Modify: `src/9p/axl-9p-internal.h` (add framing decls), `src/9p/axl-9p-codec.c` (impl), `test/unit/axl-test-9p.c` (golden-bytes test)

**Interfaces:**
- Produces: `void axl_9p_msg_begin(Axl9pWriter *w, uint8_t *buf, size_t cap, uint8_t type, uint16_t tag)` — reserves `size[4]`, writes `type[1] tag[2]`. `size_t axl_9p_msg_finish(Axl9pWriter *w)` — patches `size[4]` = total length, returns it. `bool axl_9p_msg_header(Axl9pReader *r, uint32_t *size, uint8_t *type, uint16_t *tag)` — reads the 7-byte header.

- [ ] **Step 1: Declare the framing functions** in `src/9p/axl-9p-internal.h` (below the primitive decls):

```c
void   axl_9p_msg_begin(Axl9pWriter *w, uint8_t *buf, size_t cap,
                        uint8_t type, uint16_t tag);
size_t axl_9p_msg_finish(Axl9pWriter *w);
bool   axl_9p_msg_header(Axl9pReader *r, uint32_t *size,
                         uint8_t *type, uint16_t *tag);
```

- [ ] **Step 2: Write the failing golden-bytes test** (append to `test/unit/axl-test-9p.c`, and add to `TEST_MAIN`):

```c
static void
test_tversion_golden(void)
{
    /* Tversion{ tag=0xffff, msize=8192, version="9P2000.L" } has a fixed
       21-byte wire encoding. Pin it exactly (endianness + field order). */
    uint8_t buf[64];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, buf, sizeof(buf), AXL_9P_TVERSION, 0xFFFF);
    axl_9p_w_u32(&w, AXL_9P_MSIZE);
    axl_9p_w_str(&w, "9P2000.L");
    size_t total = axl_9p_msg_finish(&w);

    static const uint8_t want[] = {
        0x15, 0x00, 0x00, 0x00,           /* size = 21 */
        0x64,                             /* type = Tversion (100) */
        0xFF, 0xFF,                       /* tag  = 0xffff */
        0x00, 0x20, 0x00, 0x00,           /* msize = 8192 */
        0x08, 0x00,                       /* strlen = 8 */
        '9','P','2','0','0','0','.','L'
    };
    test_check(total == sizeof(want), "9p frame: Tversion length is 21");
    test_check(axl_memcmp(buf, want, sizeof(want)) == 0,
               "9p frame: Tversion golden bytes match");

    /* Header parse round-trips. */
    Axl9pReader r;
    axl_9p_r_init(&r, buf, total);
    uint32_t sz; uint8_t ty; uint16_t tag;
    test_check(axl_9p_msg_header(&r, &sz, &ty, &tag),
               "9p frame: header parses");
    test_check(sz == 19 && ty == AXL_9P_TVERSION && tag == 0xFFFF,
               "9p frame: header fields correct");
}
```

- [ ] **Step 3: Run to verify FAIL**

Run: `make ARCH=x64 tests 2>&1 | grep -iE 'axl_9p_msg_begin|undefined'`
Expected: FAIL — `undefined reference to 'axl_9p_msg_begin'`.

- [ ] **Step 4: Implement framing** (append to `src/9p/axl-9p-codec.c`):

```c
void
axl_9p_msg_begin(Axl9pWriter *w, uint8_t *buf, size_t cap,
                 uint8_t type, uint16_t tag)
{
    axl_9p_w_init(w, buf, cap);
    axl_9p_w_u32(w, 0);        /* size placeholder — patched by _finish */
    axl_9p_w_u8(w, type);
    axl_9p_w_u16(w, tag);
}

size_t
axl_9p_msg_finish(Axl9pWriter *w)
{
    uint32_t sz = (uint32_t)w->len;
    if (w->cap >= 4) {
        w->buf[0] = (uint8_t)sz;         w->buf[1] = (uint8_t)(sz >> 8);
        w->buf[2] = (uint8_t)(sz >> 16); w->buf[3] = (uint8_t)(sz >> 24);
    }
    return w->len;
}

bool
axl_9p_msg_header(Axl9pReader *r, uint32_t *size,
                  uint8_t *type, uint16_t *tag)
{
    *size = axl_9p_r_u32(r);
    *type = axl_9p_r_u8(r);
    *tag  = axl_9p_r_u16(r);
    return !r->error;
}
```

- [ ] **Step 5: Run to verify PASS**

Run: `TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh --arch X64 2>&1 | grep -iE '9p frame|Results:'`
Expected: all `9p frame:` checks PASS.

- [ ] **Step 6: Both-arch check + commit**

Run: `TEST_APPS_ONLY=AxlTest9p ./test/integration/test-axl.sh --arch AARCH64 2>&1 | grep 'Results:'`
Expected: `Results: N passed, 0 failed`.

```bash
git add src/9p/axl-9p-internal.h src/9p/axl-9p-codec.c test/unit/axl-test-9p.c
git commit -m "9p: message framing (begin/finish/header) + Tversion golden-bytes test"
```

---

## Task 3: Public header + client struct + transact helper

No behavior test of its own — it defines the public surface and the private connection state consumed by Tasks 4-6. It ends by compiling clean (the header must build under both C and C++ include paths per the SDK contract).

**Files:**
- Create: `include/axl/axl-9p.h`, `src/9p/axl-9p-client.c`
- Modify: `src/9p/axl-9p-internal.h` (add `Axl9pClient` struct + transact decl), `Makefile` (install `axl-9p.h`)

**Interfaces:**
- Produces (public): `typedef struct Axl9pClient Axl9pClient;` and the Phase-1 client functions (bodies land in Tasks 4-6). Produces (internal): the `Axl9pClient` struct and `int axl_9p_transact(Axl9pClient *c, const uint8_t *req, size_t req_len, uint8_t expect_type)` returning `AXL_OK` with the reply parsed into `c->rbuf`/`c->rlen` (reader positioned past the header), or `AXL_ERR` on I/O error / `Rlerror` / wrong type.

- [ ] **Step 1: Create the public header** `include/axl/axl-9p.h`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p.h
    9P2000.L client for UEFI. Connect to a 9P server over TCP and read files
    and directories. Uses standard C types only; no EDK2 types leak.
**/

#ifndef AXL_9P_H
#define AXL_9P_H

#include <axl/axl-macros.h>
#include <axl/axl-bytes.h>
#include <axl/axl-array.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief List a directory's entries.
 *
 * Walks to @p path, opens it, and reads all directory entries.
 *
 * @return AXL_OK on success (@p out receives an AxlArray of AxlFsEntry the
 *     caller frees with axl_array_free); AXL_ERR on a missing / non-directory
 *     path or NULL arg.
 */
int
axl_9p_list(
    Axl9pClient *c,       ///< connected session
    const char  *path,    ///< absolute directory path on the server
    AxlArray   **out      ///< [out] AxlArray of AxlFsEntry
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_9P_H */
```

- [ ] **Step 2: Add the client struct + transact decl** to `src/9p/axl-9p-internal.h`:

```c
#include <axl/axl-tcp.h>     /* AxlTcp */

struct Axl9pClient {
    AxlTcp  *sock;
    uint32_t msize;        ///< negotiated max message size
    uint32_t root_fid;     ///< fid attached to the tree root (0)
    uint32_t next_fid;     ///< monotonic fid allocator (root_fid + 1 ..)
    uint8_t  rbuf[AXL_9P_MSIZE];   ///< last reply
    size_t   rlen;         ///< length of the last reply
};

/* Send req[req_len], receive one reply into c->rbuf. On AXL_OK the reply
   type equals expect_type; an Rlerror or a type mismatch yields AXL_ERR. */
int axl_9p_transact(Axl9pClient *c, const uint8_t *req, size_t req_len,
                    uint8_t expect_type);
```

> If `AXL_9P_MSIZE` (8 KiB) inline buffers make the struct too large for a
> static/stack context later, switch `rbuf` to a heap allocation in Task 4; for
> Phase 1 the inline buffer keeps the code simple.

- [ ] **Step 3: Create `src/9p/axl-9p-client.c` with the transact helper only** (functions from Tasks 4-6 append here):

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-client.c
    Synchronous 9P2000.L client over AxlTcp: connect, read_file, list.
**/

#include <axl/axl-9p.h>
#include "axl-9p-internal.h"
#include <axl/axl-tcp.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("9p");

#define AXL_9P_IO_TIMEOUT_MS  5000u

/* Read exactly n bytes (sync recv returns partial). */
static int
recv_exact(AxlTcp *sock, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t chunk = n - got;
        if (axl_tcp_recv(sock, buf + got, &chunk, AXL_9P_IO_TIMEOUT_MS) != AXL_OK
            || chunk == 0) {
            return AXL_ERR;
        }
        got += chunk;
    }
    return AXL_OK;
}

int
axl_9p_transact(Axl9pClient *c, const uint8_t *req, size_t req_len,
                uint8_t expect_type)
{
    if (axl_tcp_send(c->sock, req, req_len, AXL_9P_IO_TIMEOUT_MS) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t hdr[7];
    if (recv_exact(c->sock, hdr, 7) != AXL_OK) {
        return AXL_ERR;
    }
    Axl9pReader hr;
    axl_9p_r_init(&hr, hdr, 7);
    uint32_t size; uint8_t type; uint16_t tag;
    axl_9p_msg_header(&hr, &size, &type, &tag);
    if (size < 7 || size > sizeof(c->rbuf)) {
        return AXL_ERR;
    }
    axl_memcpy(c->rbuf, hdr, 7);
    if (size > 7 && recv_exact(c->sock, c->rbuf + 7, size - 7) != AXL_OK) {
        return AXL_ERR;
    }
    c->rlen = size;
    if (type == AXL_9P_RLERROR) {
        Axl9pReader er;
        axl_9p_r_init(&er, c->rbuf + 7, size - 7);
        uint32_t ecode = axl_9p_r_u32(&er);
        axl_warning("9p: server error errno=%u", ecode);
        return AXL_ERR;
    }
    return (type == expect_type) ? AXL_OK : AXL_ERR;
}
```

- [ ] **Step 4: Install the public header** — in `Makefile`, add `axl-9p.h` wherever `include/axl/*.h` public headers are staged for the SDK (follow the existing header-install list; look for `axl-tcp.h` and add `axl-9p.h` beside it).

- [ ] **Step 5: Verify it builds clean (both arches, no warnings)**

Run: `make ARCH=x64 2>&1 | grep -iE 'warning|error' | grep -v '^gcc\|^ar '; echo rc=$?`
Then: `make ARCH=aa64 2>&1 | grep -iE 'warning|error' | grep -v '^gcc\|^ar '`
Expected: no warnings/errors (the new .c files compile; unresolved public functions are fine — nothing references them yet).

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-9p.h src/9p/axl-9p-internal.h src/9p/axl-9p-client.c Makefile
git commit -m "9p: public <axl/axl-9p.h> client surface + connection struct + transact helper"
```

---

## Task 4: `axl_9p_connect` + client integration harness

Infra-first for the integration harness (host server + QEMU driver mode), then the connect assertion.

**Files:**
- Create: `test/integration/p9-server.py`, `test/integration/test-9p-qemu.sh`
- Modify: `src/9p/axl-9p-client.c` (add `axl_9p_connect` / `axl_9p_disconnect`), `test/unit/axl-test-net.c` (add `9p-client <host> <port>` mode)

**Interfaces:**
- Consumes: `axl_9p_transact`, codec primitives, framing.
- Produces: `axl_9p_connect`, `axl_9p_disconnect`; serial marker `9P-CONNECT-OK` from the test mode.

- [ ] **Step 1: Write the host 9P server** `test/integration/p9-server.py` — a read+list 9P2000.L server over TCP serving a fixed tree (`/hello.txt` = `b"hello from 9p\n"`, `/dir/{a.txt,b.txt}`). Port the spike's `p9-spike-server.py` structure (Tversion/Tattach/Twalk/Tlopen/Tread/Treaddir/Tclunk + Rlerror). Follow the Python standards in the user CLAUDE.md (`from __future__ import annotations`, type hints, `if __name__ == "__main__":`). Keep it self-contained (stdlib `socket`/`struct` only).

> The spike server `test/integration/p9-spike-server.py` is a correct, tested
> starting point — reproduce its logic here (this file survives; the spike file
> is deleted in Task 8).

- [ ] **Step 2: Add the `9p-client` driver mode** to `test/unit/axl-test-net.c` (near the other client modes; register in the `test_net_main` dispatch, e.g. `if (argc >= 4 && axl_strcmp(argv[1], "9p-client") == 0) return run_9p_client_mode(argv[2], argv[3]);`):

```c
static int
run_9p_client_mode(const char *host, const char *port_str)
{
    uint16_t port;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0 || port == 0) {
        axl_printf("9P-CLIENT-FAIL:port\n");
        return 1;
    }
    axl_net_auto_init(SIZE_MAX, 10);

    Axl9pClient *c = NULL;
    if (axl_9p_connect(host, port, "axl", "/", &c) != AXL_OK) {
        axl_printf("9P-CLIENT-FAIL:connect\n");
        return 1;
    }
    axl_printf("9P-CONNECT-OK\n");
    /* read_file + list assertions arrive in Tasks 5 and 6. */
    axl_9p_disconnect(c);
    axl_printf("9P-CLIENT-OK\n");
    return 0;
}
```

Add `#include <axl/axl-9p.h>` to `axl-test-net.c` if not already present.

- [ ] **Step 3: Write the integration test** `test/integration/test-9p-qemu.sh` — model on the spike's `test-9p-spike-qemu.sh`: start `p9-server.py` on a host port, boot QEMU with `AxlTestNet.efi 9p-client 10.0.2.2 <port>` in `startup.nsh` (DHCP first, `reset -s` last), `TEST_SKIP_RATCHET=1`. Assert the serial log contains `9P-CONNECT-OK` and `9P-CLIENT-OK`. Make it executable (`chmod +x`).

- [ ] **Step 4: Run the integration test to verify it FAILS**

Run: `make ARCH=x64 tests >/dev/null 2>&1; timeout 150 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep -iE '9P-|PASS:|FAIL:'`
Expected: FAIL — `axl_9p_connect` is undefined at link OR (if it links as a stub) `9P-CLIENT-FAIL:connect`. If it fails to link, that is the RED.

- [ ] **Step 5: Implement `axl_9p_connect` / `axl_9p_disconnect`** (append to `src/9p/axl-9p-client.c`):

```c
int
axl_9p_connect(const char *host, uint16_t port, const char *uname,
               const char *aname, Axl9pClient **out)
{
    if (host == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (uname == NULL) uname = "";
    if (aname == NULL || aname[0] == '\0') aname = "/";

    Axl9pClient *c = axl_new(Axl9pClient);
    if (c == NULL) {
        return AXL_ERR;
    }
    c->msize    = AXL_9P_MSIZE;
    c->root_fid = 0;
    c->next_fid = 1;

    if (axl_tcp_connect_timeout(host, port, NULL, 4000, &c->sock) != AXL_OK
        || c->sock == NULL) {
        axl_free(c);
        return AXL_ERR;
    }

    /* Tversion(msize, "9P2000.L") -> Rversion */
    uint8_t req[64];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TVERSION, 0xFFFF);
    axl_9p_w_u32(&w, AXL_9P_MSIZE);
    axl_9p_w_str(&w, "9P2000.L");
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RVERSION) != AXL_OK) {
        goto fail;
    }
    {   /* Rversion: msize[4] version[s] — require exact "9P2000.L". */
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t smsize = axl_9p_r_u32(&r);
        char ver[16];
        axl_9p_r_str(&r, ver, sizeof(ver));
        if (r.error || axl_strcmp(ver, "9P2000.L") != 0) {
            axl_warning("9p: server declined 9P2000.L (got '%s')", ver);
            goto fail;
        }
        if (smsize > 0 && smsize < c->msize) {
            c->msize = smsize;
        }
    }

    /* Tattach(root_fid, NOFID, uname, aname, NONUNAME) -> Rattach */
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TATTACH, 0);
    axl_9p_w_u32(&w, c->root_fid);
    axl_9p_w_u32(&w, AXL_9P_NOFID);
    axl_9p_w_str(&w, uname);
    axl_9p_w_str(&w, aname);
    axl_9p_w_u32(&w, AXL_9P_NONUNAME);
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RATTACH) != AXL_OK) {
        goto fail;
    }

    *out = c;
    return AXL_OK;

fail:
    axl_tcp_close(c->sock);
    axl_free(c);
    return AXL_ERR;
}

void
axl_9p_disconnect(Axl9pClient *c)
{
    if (c == NULL) {
        return;
    }
    axl_tcp_close(c->sock);
    axl_free(c);
}
```

> `req[64]` bounds the Tattach: `uname`/`aname` are short in tests. Task 5/6 use a
> larger request buffer for walks with long paths — size those `req` buffers to
> `msize` or validate `!w.overflow` (already done) and fail cleanly.

- [ ] **Step 6: Run the integration test to verify it PASSES**

Run: `make ARCH=x64 tests >/dev/null 2>&1; timeout 150 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep -iE '9P-CONNECT-OK|9P-CLIENT-OK|passed,'`
Expected: `9P-CONNECT-OK`, `9P-CLIENT-OK`, and a `passed, 0 failed` footer.

- [ ] **Step 7: Commit**

```bash
git add src/9p/axl-9p-client.c test/unit/axl-test-net.c \
        test/integration/p9-server.py test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_connect/disconnect (version+attach) + client integration harness"
```

---

## Task 5: `axl_9p_read_file` (walk → lopen → chunked read)

**Files:**
- Modify: `src/9p/axl-9p-client.c` (add `axl_9p_read_file` + a static `walk`/`clunk` helper), `test/unit/axl-test-net.c` (extend `9p-client` mode)

**Interfaces:**
- Consumes: `axl_9p_connect`, `axl_9p_transact`, codec.
- Produces: `axl_9p_read_file`; internal statics `client_walk(c, path, fid)` and `client_clunk(c, fid)`.

- [ ] **Step 1: Extend the driver mode to read /hello.txt** — in `run_9p_client_mode`, after `9P-CONNECT-OK`, before disconnect:

```c
    AxlBytes *fb = NULL;
    if (axl_9p_read_file(c, "/hello.txt", &fb) == AXL_OK && fb != NULL) {
        size_t n = 0;
        const uint8_t *d = axl_bytes_get_data(fb, &n);
        axl_printf("9P-READ:%.*s\n", (int)n, (const char *)d);
        axl_bytes_unref(fb);
    } else {
        axl_printf("9P-READ-FAIL\n");
    }
```

Add the matching assertion to `test-9p-qemu.sh`: serial must contain `9P-READ:hello from 9p`.

- [ ] **Step 2: Run to verify FAIL**

Run: `make ARCH=x64 tests 2>&1 | grep -iE 'axl_9p_read_file|undefined'`
Expected: FAIL — `undefined reference to 'axl_9p_read_file'`.

- [ ] **Step 3: Implement the walk/clunk helpers and read_file** (append to `src/9p/axl-9p-client.c`):

```c
/* Split an absolute '/'-path into components and Twalk root_fid -> new_fid.
   Returns AXL_OK with *out_fid a fresh fid pointing at path (a clone of root
   for "/" — zero wname elements). Caller clunks *out_fid when done. */
static int
client_walk(Axl9pClient *c, const char *path, uint32_t *out_fid)
{
    uint32_t new_fid = c->next_fid++;
    uint8_t  req[AXL_9P_MSIZE];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TWALK, 0);
    axl_9p_w_u32(&w, c->root_fid);
    axl_9p_w_u32(&w, new_fid);

    /* Count + emit path components (skip leading '/' and empty segments). */
    size_t nw_pos = w.len;          /* remember where nwname[2] goes */
    axl_9p_w_u16(&w, 0);            /* placeholder count, patched below */
    uint16_t nwname = 0;
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        const char *start = p;
        while (*p != '\0' && *p != '/') p++;
        size_t seg = (size_t)(p - start);
        if (seg == 0) continue;
        char comp[256];
        size_t clen = (seg < sizeof(comp) - 1) ? seg : sizeof(comp) - 1;
        axl_memcpy(comp, start, clen);
        comp[clen] = '\0';
        axl_9p_w_str(&w, comp);
        nwname++;
    }
    /* Patch nwname[2] little-endian. */
    req[nw_pos]     = (uint8_t)nwname;
    req[nw_pos + 1] = (uint8_t)(nwname >> 8);

    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RWALK) != AXL_OK) {
        return AXL_ERR;
    }
    /* Rwalk: nwqid[2] — must equal nwname (full walk succeeded). */
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint16_t nwqid = axl_9p_r_u16(&r);
    if (r.error || nwqid != nwname) {
        return AXL_ERR;
    }
    *out_fid = new_fid;
    return AXL_OK;
}

static void
client_clunk(Axl9pClient *c, uint32_t fid)
{
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TCLUNK, 0);
    axl_9p_w_u32(&w, fid);
    (void)axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RCLUNK);
}

/* Tlopen(fid, flags) -> Rlopen. flags: 0 = O_RDONLY. */
static int
client_lopen(Axl9pClient *c, uint32_t fid, uint32_t flags)
{
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TLOPEN, 0);
    axl_9p_w_u32(&w, fid);
    axl_9p_w_u32(&w, flags);
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RLOPEN);
}

int
axl_9p_read_file(Axl9pClient *c, const char *path, AxlBytes **out)
{
    if (c == NULL || path == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (client_lopen(c, fid, 0) != AXL_OK) {
        goto done;
    }

    /* Accumulate chunked Treads into a growable buffer. */
    AxlBytes *acc = axl_bytes_new(NULL, 0);
    uint64_t offset = 0;
    uint32_t chunk  = c->msize - 11;          /* header(7) + count(4) */
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREAD, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, chunk);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREAD) != AXL_OK) {
            axl_bytes_unref(acc);
            goto done;
        }
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t count = axl_9p_r_u32(&r);
        if (r.error || 7 + 4 + count > c->rlen) {
            axl_bytes_unref(acc);
            goto done;
        }
        if (count == 0) {
            break;                             /* EOF */
        }
        AxlBytes *piece = axl_bytes_new(c->rbuf + 11, count);
        AxlBytes *joined = axl_bytes_concat(acc, piece);   /* see note */
        axl_bytes_unref(piece);
        axl_bytes_unref(acc);
        acc = joined;
        offset += count;
    }
    *out = acc;
    rc = AXL_OK;

done:
    client_clunk(c, fid);
    return rc;
}
```

> **Verify the concat/unref API names before implementing.** This plan assumes
> `axl_bytes_new(data,len)`, `axl_bytes_get_data(b,&n)`, and an unref/free
> (`axl_bytes_unref`). If there is no `axl_bytes_concat`, accumulate into an
> `AxlStrBuf`/growable byte buffer instead (grep `include/axl/axl-bytes.h` and
> `axl-strbuf.h`), then materialize one `AxlBytes` at the end. Adjust the two
> `concat` lines accordingly; the rest is unchanged.

- [ ] **Step 4: Run to verify PASS**

Run: `make ARCH=x64 tests >/dev/null 2>&1; timeout 150 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep -iE '9P-READ|passed,'`
Expected: `9P-READ:hello from 9p` and `passed, 0 failed`.

- [ ] **Step 5: Refactor while green + commit**

```bash
git add src/9p/axl-9p-client.c test/unit/axl-test-net.c test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_read_file (walk + lopen + chunked Tread)"
```

---

## Task 6: `axl_9p_list` (walk → lopen → Treaddir → AxlArray<AxlFsEntry>)

**Files:**
- Modify: `src/9p/axl-9p-client.c` (add `axl_9p_list`), `test/unit/axl-test-net.c` (extend `9p-client` mode)

**Interfaces:**
- Consumes: `client_walk`, `client_lopen`, `client_clunk`, codec.
- Produces: `axl_9p_list` → `AxlArray` of `AxlFsEntry`.

- [ ] **Step 1: Extend the driver mode to list /dir** — after the read block:

```c
    AxlArray *entries = NULL;
    if (axl_9p_list(c, "/dir", &entries) == AXL_OK && entries != NULL) {
        axl_printf("9P-LIST:");
        for (size_t i = 0; i < axl_array_len(entries); i++) {
            AxlFsEntry *e = (AxlFsEntry *)axl_array_get(entries, i);
            axl_printf("%s,", e->name);
        }
        axl_printf("\n");
        axl_array_free(entries);
    } else {
        axl_printf("9P-LIST-FAIL\n");
    }
```

Add the assertion to `test-9p-qemu.sh`: the `9P-LIST:` line must contain `a.txt` and `b.txt`.

- [ ] **Step 2: Run to verify FAIL**

Run: `make ARCH=x64 tests 2>&1 | grep -iE 'axl_9p_list|undefined'`
Expected: FAIL — `undefined reference to 'axl_9p_list'`.

- [ ] **Step 3: Implement `axl_9p_list`** (append to `src/9p/axl-9p-client.c`; add `#include <axl/axl-fs.h>` at the top for `AxlFsEntry`):

```c
int
axl_9p_list(Axl9pClient *c, const char *path, AxlArray **out)
{
    if (c == NULL || path == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (client_lopen(c, fid, 0) != AXL_OK) {
        goto done;
    }

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint64_t offset = 0;
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREADDIR, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, c->msize - 11);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREADDIR) != AXL_OK) {
            axl_array_free(arr);
            goto done;
        }
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t dcount = axl_9p_r_u32(&r);
        if (r.error || dcount == 0) {
            break;                     /* end of directory */
        }
        /* Each entry: qid[13] offset[8] type[1] name[s]. */
        size_t start = r.pos;
        while (r.pos - start < dcount && !r.error) {
            r.pos += AXL_9P_QID_LEN;                 /* skip qid */
            uint64_t next_off = axl_9p_r_u64(&r);    /* dirent offset cursor */
            uint8_t  dtype    = axl_9p_r_u8(&r);     /* DT_DIR=4, DT_REG=8 */
            AxlFsEntry e = {0};
            e.struct_size = sizeof(AxlFsEntry);
            e.version     = AXL_FS_ENTRY_VERSION;
            axl_9p_r_str(&r, e.name, sizeof(e.name));
            if (dtype == 4) {           /* DT_DIR */
                e.attributes |= AXL_FS_ATTR_DIRECTORY;
            }
            if (r.error) {
                break;
            }
            /* Skip "." and ".." — not surfaced as volume entries. */
            if (axl_strcmp(e.name, ".") != 0 && axl_strcmp(e.name, "..") != 0) {
                axl_array_append(arr, &e);
            }
            offset = next_off;
        }
    }
    *out = arr;
    rc = AXL_OK;

done:
    client_clunk(c, fid);
    return rc;
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `make ARCH=x64 tests >/dev/null 2>&1; timeout 150 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep -iE '9P-LIST|passed,'`
Expected: `9P-LIST:a.txt,b.txt,` (order may vary) and `passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/9p/axl-9p-client.c test/unit/axl-test-net.c test/integration/test-9p-qemu.sh
git commit -m "9p: axl_9p_list (Treaddir -> AxlArray of AxlFsEntry)"
```

---

## Task 7: Both-arch validation, docs, module registration

**Files:**
- Create: `src/9p/README.md`
- Modify: `docs/sphinx/modules/net.rst` (add `.. doxygenfile:: axl-9p.h`), `docs/sphinx/index.rst` (only if a new page is warranted — otherwise fold into the net page), `CLAUDE.md` (module table row + Project Layout), `include/axl.h` (add `#include <axl/axl-9p.h>` to the umbrella if other public headers are listed there)

- [ ] **Step 1: Full unit suite, both arches**

Run: `./test/integration/test-axl.sh --arch X64 2>&1 | tail -2`
Then: `./test/integration/test-axl.sh --arch AARCH64 2>&1 | tail -2`
Expected: `Results: N passed, 0 failed` on both (N = prior baseline + the AxlTest9p codec checks). Update `test/integration/.last-pass-count` if the runner does not auto-bump it.

- [ ] **Step 2: Integration test, both arches**

Run: `timeout 200 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep 'passed,'`
Then: `timeout 200 ./test/integration/test-9p-qemu.sh --arch AARCH64 2>&1 | grep 'passed,'`
Expected: `passed, 0 failed` on both.

- [ ] **Step 3: Write `src/9p/README.md`** — module intro: what 9P2000.L is, the client-in-Phase-1 scope, the API surface, and a one-paragraph usage example (`axl_9p_connect` → `axl_9p_read_file` → `axl_9p_disconnect`). Prose must not over-claim (no server/mount yet).

- [ ] **Step 4: Add the module to docs + CLAUDE.md** — add `.. doxygenfile:: axl-9p.h` under the appropriate Sphinx module page and a row to the CLAUDE.md module table (`Axl9p | src/9p/ | axl/axl-9p.h`). Run the doc-coverage gate:

Run: `make check-docs 2>&1 | tail -2` and `make check-ascii 2>&1 | tail -1`
Expected: both `clean`.

- [ ] **Step 5: Commit**

```bash
git add src/9p/README.md docs/ CLAUDE.md include/axl.h test/integration/.last-pass-count
git commit -m "9p: docs (README + Sphinx + module table) for the Phase 1 client"
```

---

## Task 8: Delete the throwaway spike

The spike proved feasibility and is superseded by the productized codec/client. Remove it so it can't rot or confuse.

**Files:**
- Delete: `test/integration/p9-spike-server.py`, `test/integration/test-9p-spike-qemu.sh`
- Modify: `test/unit/axl-test-net.c` (remove `run_9p_spike_mode` + its `9p-spike` dispatch + the `P9_*`/`p9_*` spike helpers)

- [ ] **Step 1: Remove the spike code + files**

```bash
git rm test/integration/p9-spike-server.py test/integration/test-9p-spike-qemu.sh
```
Then edit `test/unit/axl-test-net.c`: delete the spike block (the `#define P9_*`, the `p9_*` static helpers, `run_9p_spike_mode`, and the `9p-spike` dispatch line). Leave the productized `9p-client` mode intact.

- [ ] **Step 2: Verify the build + the 9P integration test still pass (x64)**

Run: `make ARCH=x64 tests 2>&1 | tail -1; timeout 150 ./test/integration/test-9p-qemu.sh --arch X64 2>&1 | grep 'passed,'`
Expected: builds clean; `passed, 0 failed`.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "9p: remove the feasibility spike (superseded by the productized client)"
```

---

## Self-Review Notes (author)

- **Spec coverage (Phase 1 rows of §12):** codec (Tasks 1-2), client core connect/read_file/list (Tasks 4-6), codec unit tests (Tasks 1-2), client integration (Tasks 4-6), spike discard (Task 8). Server/write/mount are later phases, intentionally absent.
- **Type consistency:** `Axl9pClient`, `Axl9pWriter`, `Axl9pReader`, `axl_9p_transact`, `client_walk`/`client_lopen`/`client_clunk` names are used identically across Tasks 3-6. `AxlFsEntry` fields (`struct_size`, `version`, `name`, `attributes`) match `include/axl/axl-fs.h`.
- **Two verify-before-implementing notes** are flagged inline (Task 1 Step 2: test-harness macro; Task 5 Step 3: `AxlBytes` concat/unref API). These are real "confirm the exact SDK symbol" checks, not placeholders — the implementer greps the named header and adapts the 1-2 lines.
```
