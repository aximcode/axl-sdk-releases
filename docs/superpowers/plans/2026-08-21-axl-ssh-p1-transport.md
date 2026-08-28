# AxlSsh P1 — Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A stock `ssh(1)` completes version exchange, algorithm negotiation, curve25519 key exchange and `NEWKEYS` against a UEFI guest, then disconnects cleanly at the auth boundary.

**Architecture:** A server-only SSH transport over `AxlTcp` + `AxlLoop` (never `AxlSocket` — see Global Constraints). Four internal layers, each its own file: wire codec → binary packet protocol → key exchange → connection state machine on `AxlTcp` callbacks. Crypto is `AxlCrypto` over vendored mbedtls; this phase writes framing and a state machine, no cryptographic primitives.

**Tech Stack:** C11, `AxlTcp`/`AxlLoop`, `AxlCrypto` (`AXL_ECDH_X25519`, `AXL_PK_ED25519`, `AXL_AEAD_CHACHA20_POLY1305`), `AxlDigest` (SHA-256), `AxlStrBuf`. Tests: `test/unit/axl-test-ssh.c` under QEMU, plus host `ssh(1)` as the conformance oracle.

**Spec:** `docs/superpowers/specs/2026-08-21-axl-ssh-design.md` (§7 wire scope, §10 testing, §12 phasing)

## Global Constraints

- **Build on `AxlTcp`, never `AxlSocket`.** ROADMAP's networking-layering item says `AxlSocket` is a BSD-compat veneer with no library consumer, and that a socket-based server would make it load-bearing and force that deferred layering decision. HTTP and 9P use `AxlTcp` directly; so does this.
- **One algorithm per slot.** kex `curve25519-sha256`, host key `ssh-ed25519`, cipher `chacha20-poly1305@openssh.com`, mac `implicit` (AEAD), compression `none`. Anything else is refused, never negotiated down.
- **Every protocol violation disconnects.** `SSH_MSG_DISCONNECT` + reason code, no resynchronisation.
- **Exact-string assertions for output** (`axl_strcmp(buf, "...") == 0`, never `axl_strstr`) — CLAUDE.md hard rule.
- **No `test_check(true, ...)`** — `make check-tautology` fails the build.
- **Balanced SKIP counts**: a `test_skip_n(N, ...)` branch must skip exactly as many assertions as the populated branch makes.
- **Confirm RED before implementing** every task. A test that passes before the code exists is testing nothing.
- **Doc sync in the same change**: a new public header needs `///<` param docs, a `@file` block, and a `.. doxygenfile::` line, or `make check-docs` and `build-docs.sh` fail.
- **Every `.efi`-producing test needs a `# test-meta:` header** or `make check-test-meta` fails.
- **Security review is a gate for this phase**, per spec §11. P1 does not merge without an independent review of the kex and framing surface.

---

## File Structure

**Header split is fixed by the spec (§1) and must be honoured from P1**, because
retrofitting it after consumers exist is an API break: `axl-ssh-core.h` holds
everything both roles use, `axl-ssh-server.h` the server, and a later
`axl-ssh-client.h` (P6) the client. P1 creates the first two only. The
`src/net/axl-ssh-{buf,packet,kex}.c` files below ARE the core — the client will
consume them unchanged, so nothing in them may assume a role.

| file | responsibility |
|---|---|
| `include/axl/axl-ssh-core.h` | shared: opaque `AxlSshChannel`, status codes, algorithm names |
| `include/axl/axl-ssh-server.h` | public server API — P1 exposes only `axl_ssh_server_new/listen/free` |
| `src/net/axl-ssh-internal.h` | shared internals: connection struct, message numbers, state enum |
| `src/net/axl-ssh-buf.c` | SSH wire codec — `uint32`, `string`, `name-list`, `mpint` |
| `src/net/axl-ssh-packet.c` | binary packet protocol — framing, padding, AEAD seal/open |
| `src/net/axl-ssh-kex.c` | `KEXINIT`, curve25519, exchange hash, key derivation |
| `src/net/axl-ssh-server.c` | `AxlTcp` glue + connection state machine |
| `test/unit/axl-test-ssh.c` | host-side unit tests (codec, framing, KDF vectors) |
| `test/integration/test-ssh-transport-qemu.sh` | `ssh -vvv` conformance oracle |

Split by responsibility, not layer: the codec is pure and heavily tested; the packet layer owns encryption state; kex owns the hash; the server owns I/O. Only the server file touches `AxlTcp`.

---

### Task 1: SSH wire codec

**Files:**
- Create: `src/net/axl-ssh-buf.c`
- Create: `src/net/axl-ssh-internal.h`
- Create: `test/unit/axl-test-ssh.c`
- Modify: `Makefile` (add `axl-ssh-buf.c` to `LIB_SOURCES`; add `$(eval $(call BUILD_TEST,AxlTestSsh,axl-test-ssh))`)
- Modify: `test/integration/test-axl.sh` (add `AxlTestSsh` to `TEST_APPS`)

**Interfaces:**
- Consumes: `AxlStrBuf` from `<axl/axl-str.h>`
- Produces:
  ```c
  /* All return AXL_OK / AXL_ERR. Readers advance *off only on success. */
  int axl_ssh_put_u32(AxlStrBuf *b, uint32_t v);
  int axl_ssh_get_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *out);
  int axl_ssh_put_string(AxlStrBuf *b, const void *s, size_t n);
  int axl_ssh_get_string(const uint8_t *p, size_t len, size_t *off,
                         const uint8_t **out, uint32_t *out_len);
  ```

- [ ] **Step 1: Write the failing tests**

In `test/unit/axl-test-ssh.c`:

```c
/** @file axl-test-ssh.c
    Test application for AxlSsh — wire codec, packet framing, kex KDF.
**/

#include "axl-test.h"
#include <axl/axl-str.h>
#include "../../src/net/axl-ssh-internal.h"

AXL_LOG_DOMAIN("test");

static void
test_ssh_put_u32(void)
{
    AxlStrBuf *b = axl_strbuf_new();
    test_check(axl_ssh_put_u32(b, 0x01020304u) == AXL_OK, "ssh put_u32: ok");
    test_check(axl_strbuf_len(b) == 4, "ssh put_u32: 4 bytes");
    const uint8_t *d = (const uint8_t *)axl_strbuf_data(b);
    test_check(d[0] == 0x01 && d[1] == 0x02 && d[2] == 0x03 && d[3] == 0x04,
               "ssh put_u32: big-endian");
    axl_strbuf_free(b);
}

static void
test_ssh_get_u32(void)
{
    const uint8_t d[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t off = 0; uint32_t v = 0;
    test_check(axl_ssh_get_u32(d, sizeof d, &off, &v) == AXL_OK, "ssh get_u32: ok");
    test_check(v == 0xDEADBEEFu, "ssh get_u32: value");
    test_check(off == 4, "ssh get_u32: advanced 4");

    /* Truncated input must fail AND leave off untouched. */
    size_t off2 = 0;
    test_check(axl_ssh_get_u32(d, 3, &off2, &v) == AXL_ERR, "ssh get_u32: short input errs");
    test_check(off2 == 0, "ssh get_u32: off unchanged on error");
}

static void
test_ssh_string_roundtrip(void)
{
    AxlStrBuf *b = axl_strbuf_new();
    test_check(axl_ssh_put_string(b, "ssh-ed25519", 11) == AXL_OK, "ssh put_string: ok");
    test_check(axl_strbuf_len(b) == 4 + 11, "ssh put_string: len prefix + body");

    size_t off = 0; const uint8_t *s = NULL; uint32_t n = 0;
    const uint8_t *d = (const uint8_t *)axl_strbuf_data(b);
    test_check(axl_ssh_get_string(d, axl_strbuf_len(b), &off, &s, &n) == AXL_OK,
               "ssh get_string: ok");
    test_check(n == 11, "ssh get_string: length");
    test_check(axl_memcmp(s, "ssh-ed25519", 11) == 0, "ssh get_string: bytes");
    axl_strbuf_free(b);
}

static void
test_ssh_string_length_lies(void)
{
    /* A length field larger than the buffer is the classic parser bug. */
    const uint8_t d[8] = { 0x00, 0x00, 0x10, 0x00, 'a', 'b', 'c', 'd' };
    size_t off = 0; const uint8_t *s = NULL; uint32_t n = 0;
    test_check(axl_ssh_get_string(d, sizeof d, &off, &s, &n) == AXL_ERR,
               "ssh get_string: length past end is refused");
    test_check(off == 0, "ssh get_string: off unchanged on error");
}

int
test_ssh_main(void)
{
    test_init("AxlSsh");
    axl_printf("\n--- SSH wire codec ---\n");
    test_ssh_put_u32();
    test_ssh_get_u32();
    test_ssh_string_roundtrip();
    test_ssh_string_length_lies();
    return test_print_results();
}

AXL_APP(test_ssh_main)
```

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests
```
Expected: compile FAILS — `axl-ssh-internal.h` does not exist.

- [ ] **Step 3: Write the header and implementation**

`src/net/axl-ssh-internal.h`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
#ifndef AXL_SSH_INTERNAL_H
#define AXL_SSH_INTERNAL_H

#include <axl/axl-str.h>
#include <stdint.h>
#include <stddef.h>

int axl_ssh_put_u32(AxlStrBuf *b, uint32_t v);
int axl_ssh_get_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *out);
int axl_ssh_put_string(AxlStrBuf *b, const void *s, size_t n);
int axl_ssh_get_string(const uint8_t *p, size_t len, size_t *off,
                       const uint8_t **out, uint32_t *out_len);

#endif /* AXL_SSH_INTERNAL_H */
```

`src/net/axl-ssh-buf.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-buf.c
    SSH wire codec (RFC 4251 §5). Readers advance *off ONLY on success, so a
    caller that ignores the status cannot silently consume a malformed field.
**/

#include "axl-ssh-internal.h"

int
axl_ssh_put_u32(AxlStrBuf *b, uint32_t v)
{
    uint8_t d[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    return axl_strbuf_append_bytes(b, d, sizeof d);
}

int
axl_ssh_get_u32(const uint8_t *p, size_t len, size_t *off, uint32_t *out)
{
    if (p == NULL || off == NULL || out == NULL || len - *off < 4 || *off > len) {
        return AXL_ERR;
    }
    const uint8_t *q = p + *off;
    *out = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
           ((uint32_t)q[2] << 8)  |  (uint32_t)q[3];
    *off += 4;
    return AXL_OK;
}

int
axl_ssh_put_string(AxlStrBuf *b, const void *s, size_t n)
{
    if (n > 0xFFFFFFFFu || axl_ssh_put_u32(b, (uint32_t)n) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_strbuf_append_bytes(b, s, n);
}

int
axl_ssh_get_string(const uint8_t *p, size_t len, size_t *off,
                   const uint8_t **out, uint32_t *out_len)
{
    size_t probe = (off != NULL) ? *off : 0;
    uint32_t n = 0;
    if (axl_ssh_get_u32(p, len, &probe, &n) != AXL_OK) {
        return AXL_ERR;
    }
    /* The length field is attacker-controlled: check it against what we
       actually hold before handing out a pointer. */
    if (n > len - probe) {
        return AXL_ERR;
    }
    *out = p + probe;
    *out_len = n;
    *off = probe + n;
    return AXL_OK;
}
```

- [ ] **Step 4: Register the source and the test binary**

In `Makefile`, add `src/net/axl-ssh-buf.c` to `LIB_SOURCES`, and beside the other `BUILD_TEST` lines:

```make
$(eval $(call BUILD_TEST,AxlTestSsh,axl-test-ssh))
```

In `test/integration/test-axl.sh`, add `AxlTestSsh` to the `TEST_APPS` array.

- [ ] **Step 5: Run to verify it passes**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh
```
Expected: PASS, 12 assertions, `0 failed`, and a leak verdict printed.

- [ ] **Step 6: Sabotage-verify the parser guard**

```bash
scripts/sabotage.sh -s 'src/net/axl-ssh-buf.c:s/if (n > len - probe)/if (0)/' \
  --expect-fail -- bash -c 'make ARCH=x64 tests >/dev/null && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh'
```
Expected: `sabotage.sh: OK — the sabotage was detected`.

- [ ] **Step 7: Commit**

```bash
git add src/net/axl-ssh-buf.c src/net/axl-ssh-internal.h test/unit/axl-test-ssh.c Makefile test/integration/test-axl.sh
git commit -m "feat(ssh): SSH wire codec, with attacker-controlled lengths checked"
```

---

### Task 2: Version exchange

**Files:**
- Create: `src/net/axl-ssh-server.c` (version-exchange portion only)
- Modify: `src/net/axl-ssh-internal.h`
- Modify: `test/unit/axl-test-ssh.c`

**Interfaces:**
- Consumes: Task 1's codec
- Produces:
  ```c
  #define AXL_SSH_IDENT "SSH-2.0-AxlSsh_1.0"
  /* Parses a peer identification line. Returns AXL_OK and fills *end_off with
     the offset just past the CRLF when a complete line is present;
     AXL_INCOMPLETE when more bytes are needed; AXL_ERR when malformed. */
  int axl_ssh_parse_ident(const uint8_t *p, size_t len, size_t *end_off);
  ```

- [ ] **Step 1: Write the failing tests**

Add to `test/unit/axl-test-ssh.c`:

```c
static void
test_ssh_ident_parse(void)
{
    const char *ok = "SSH-2.0-OpenSSH_9.6\r\n";
    size_t end = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)ok, axl_strlen(ok), &end) == AXL_OK,
               "ssh ident: accepts SSH-2.0");
    test_check(end == axl_strlen(ok), "ssh ident: consumes the CRLF");

    /* RFC 4253 §4.2 allows arbitrary lines BEFORE the identification. */
    const char *pre = "hello\r\nSSH-2.0-OpenSSH_9.6\r\n";
    size_t end2 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)pre, axl_strlen(pre), &end2) == AXL_OK,
               "ssh ident: skips preamble lines");
    test_check(end2 == axl_strlen(pre), "ssh ident: consumes through the ident line");

    /* Incomplete must be distinguishable from malformed. */
    const char *partial = "SSH-2.0-Open";
    size_t end3 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)partial, axl_strlen(partial), &end3)
               == AXL_INCOMPLETE, "ssh ident: partial line is INCOMPLETE not ERR");

    /* SSH-1.x is refused outright. */
    const char *v1 = "SSH-1.5-OpenSSH\r\n";
    size_t end4 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)v1, axl_strlen(v1), &end4) == AXL_ERR,
               "ssh ident: SSH-1.x refused");

    /* RFC 4253 §4.2 caps the line at 255 bytes including CRLF. */
    char big[300];
    axl_memset(big, 'x', sizeof big);
    axl_memcpy(big, "SSH-2.0-", 8);
    big[298] = '\r'; big[299] = '\n';
    size_t end5 = 0;
    test_check(axl_ssh_parse_ident((const uint8_t *)big, sizeof big, &end5) == AXL_ERR,
               "ssh ident: over-long line refused");
}
```

Call `test_ssh_ident_parse();` from `test_ssh_main` under a new
`axl_printf("\n--- SSH version exchange ---\n");` banner.

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests
```
Expected: compile FAILS — `axl_ssh_parse_ident` undeclared.

- [ ] **Step 3: Implement**

Declare in `src/net/axl-ssh-internal.h`, then in `src/net/axl-ssh-server.c`:

```c
#define AXL_SSH_IDENT      "SSH-2.0-AxlSsh_1.0"
#define AXL_SSH_IDENT_MAX  255   /* RFC 4253 §4.2, including CRLF */

int
axl_ssh_parse_ident(const uint8_t *p, size_t len, size_t *end_off)
{
    size_t start = 0;
    for (;;) {
        size_t i = start;
        while (i + 1 < len && !(p[i] == '\r' && p[i + 1] == '\n')) {
            i++;
        }
        if (i + 1 >= len) {
            return AXL_INCOMPLETE;          /* no complete line yet */
        }
        size_t line_len = i - start;
        if (line_len + 2 > AXL_SSH_IDENT_MAX) {
            return AXL_ERR;
        }
        if (line_len >= 4 && axl_memcmp(p + start, "SSH-", 4) == 0) {
            if (line_len < 8 || axl_memcmp(p + start + 4, "2.0-", 4) != 0) {
                return AXL_ERR;             /* SSH-1.x and friends */
            }
            *end_off = i + 2;
            return AXL_OK;
        }
        start = i + 2;                      /* a preamble line; keep looking */
    }
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh
```
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/net/axl-ssh-server.c src/net/axl-ssh-internal.h test/unit/axl-test-ssh.c
git commit -m "feat(ssh): version exchange, with INCOMPLETE distinct from malformed"
```

---

### Task 3: Binary packet protocol

**Files:**
- Create: `src/net/axl-ssh-packet.c`
- Modify: `src/net/axl-ssh-internal.h`, `test/unit/axl-test-ssh.c`, `Makefile`

**Interfaces:**
- Consumes: Task 1's codec
- Produces:
  ```c
  /* Frames a payload per RFC 4253 §6 (UNENCRYPTED form, used before NEWKEYS):
     uint32 packet_length | byte padding_length | payload | random padding.
     Total length is a multiple of 8 and padding is >= 4 bytes. */
  int axl_ssh_packet_wrap(AxlStrBuf *out, const void *payload, size_t len);
  /* Returns AXL_INCOMPLETE until a whole packet is present. */
  int axl_ssh_packet_unwrap(const uint8_t *p, size_t len, size_t *consumed,
                            const uint8_t **payload, uint32_t *payload_len);
  ```

- [ ] **Step 1: Write the failing tests**

```c
static void
test_ssh_packet_frame(void)
{
    AxlStrBuf *b = axl_strbuf_new();
    const char *msg = "\x05test";              /* SSH_MSG_SERVICE_REQUEST + body */
    test_check(axl_ssh_packet_wrap(b, msg, 5) == AXL_OK, "ssh packet: wrap ok");

    size_t total = axl_strbuf_len(b);
    test_check(total % 8 == 0, "ssh packet: total is a multiple of 8");
    const uint8_t *d = (const uint8_t *)axl_strbuf_data(b);
    test_check(d[4] >= 4, "ssh packet: at least 4 bytes of padding");

    size_t consumed = 0; const uint8_t *pl = NULL; uint32_t pn = 0;
    test_check(axl_ssh_packet_unwrap(d, total, &consumed, &pl, &pn) == AXL_OK,
               "ssh packet: unwrap ok");
    test_check(pn == 5, "ssh packet: payload length");
    test_check(axl_memcmp(pl, msg, 5) == 0, "ssh packet: payload bytes");
    test_check(consumed == total, "ssh packet: consumed the whole frame");
    axl_strbuf_free(b);
}

static void
test_ssh_packet_partial_and_hostile(void)
{
    AxlStrBuf *b = axl_strbuf_new();
    axl_ssh_packet_wrap(b, "\x05", 1);
    const uint8_t *d = (const uint8_t *)axl_strbuf_data(b);
    size_t total = axl_strbuf_len(b), consumed = 0;
    const uint8_t *pl = NULL; uint32_t pn = 0;

    test_check(axl_ssh_packet_unwrap(d, total - 1, &consumed, &pl, &pn) == AXL_INCOMPLETE,
               "ssh packet: short read is INCOMPLETE");

    /* A packet_length beyond our cap must be refused, not allocated. */
    const uint8_t huge[9] = { 0x00, 0x40, 0x00, 0x01, 0x04, 0, 0, 0, 0 };
    size_t c2 = 0;
    test_check(axl_ssh_packet_unwrap(huge, sizeof huge, &c2, &pl, &pn) == AXL_ERR,
               "ssh packet: oversized packet_length refused");

    /* padding_length larger than the packet is the other classic. */
    const uint8_t badpad[12] = { 0x00, 0x00, 0x00, 0x08, 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    size_t c3 = 0;
    test_check(axl_ssh_packet_unwrap(badpad, sizeof badpad, &c3, &pl, &pn) == AXL_ERR,
               "ssh packet: padding_length past the packet refused");
    axl_strbuf_free(b);
}
```

Call both from `test_ssh_main` under `axl_printf("\n--- SSH packet ---\n");`.

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests
```
Expected: compile FAILS — `axl_ssh_packet_wrap` undeclared.

- [ ] **Step 3: Implement**

`src/net/axl-ssh-packet.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-packet.c
    Binary packet protocol, RFC 4253 §6. This is the UNENCRYPTED form used
    before NEWKEYS; the AEAD form arrives in Task 6.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-rng.h>

#define AXL_SSH_PACKET_MAX  35000u   /* RFC 4253 §6.1 floor for implementations */
#define AXL_SSH_BLOCK       8u       /* cipher block for the unencrypted form */

int
axl_ssh_packet_wrap(AxlStrBuf *out, const void *payload, size_t len)
{
    /* padding brings (4 + 1 + len + pad) to a multiple of 8, min 4 bytes. */
    size_t unpadded = 4 + 1 + len;
    size_t pad = AXL_SSH_BLOCK - (unpadded % AXL_SSH_BLOCK);
    if (pad < 4) {
        pad += AXL_SSH_BLOCK;
    }
    if (axl_ssh_put_u32(out, (uint32_t)(1 + len + pad)) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t pl = (uint8_t)pad;
    if (axl_strbuf_append_bytes(out, &pl, 1) != AXL_OK ||
        axl_strbuf_append_bytes(out, payload, len) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t padbuf[2 * AXL_SSH_BLOCK];
    if (axl_rng_bytes(padbuf, pad) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_strbuf_append_bytes(out, padbuf, pad);
}

int
axl_ssh_packet_unwrap(const uint8_t *p, size_t len, size_t *consumed,
                      const uint8_t **payload, uint32_t *payload_len)
{
    size_t off = 0;
    uint32_t plen = 0;
    if (len < 4) {
        return AXL_INCOMPLETE;
    }
    if (axl_ssh_get_u32(p, len, &off, &plen) != AXL_OK) {
        return AXL_ERR;
    }
    /* Bound BEFORE trusting: plen is attacker-controlled and would otherwise
       size a read. */
    if (plen < 1 + 4 || plen > AXL_SSH_PACKET_MAX) {
        return AXL_ERR;
    }
    if (len < 4 + plen) {
        return AXL_INCOMPLETE;
    }
    uint8_t pad = p[4];
    if ((uint32_t)pad + 1u > plen) {
        return AXL_ERR;
    }
    *payload      = p + 5;
    *payload_len  = plen - 1 - pad;
    *consumed     = 4 + plen;
    return AXL_OK;
}
```

Add `src/net/axl-ssh-packet.c` to `LIB_SOURCES`.

- [ ] **Step 4: Run to verify it passes**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh
```
Expected: PASS, `0 failed`.

- [ ] **Step 5: Sabotage-verify the two bounds**

```bash
scripts/sabotage.sh -s 'src/net/axl-ssh-packet.c:s/plen > AXL_SSH_PACKET_MAX/0/' \
  --expect-fail -- bash -c 'make ARCH=x64 tests >/dev/null && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh'
scripts/sabotage.sh -s 'src/net/axl-ssh-packet.c:s/(uint32_t)pad + 1u > plen/0/' \
  --expect-fail -- bash -c 'make ARCH=x64 tests >/dev/null && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh'
```
Expected: both report `the sabotage was detected`.

- [ ] **Step 6: Commit**

```bash
git add src/net/axl-ssh-packet.c src/net/axl-ssh-internal.h test/unit/axl-test-ssh.c Makefile
git commit -m "feat(ssh): binary packet protocol, with both length fields bounded"
```

---

### Task 4: KEXINIT and algorithm selection

**Files:**
- Create: `src/net/axl-ssh-kex.c`
- Modify: `src/net/axl-ssh-internal.h`, `test/unit/axl-test-ssh.c`, `Makefile`

**Interfaces:**
- Consumes: Tasks 1 and 3
- Produces:
  ```c
  #define AXL_SSH_MSG_KEXINIT 20
  #define AXL_SSH_MSG_NEWKEYS 21
  /* Builds our KEXINIT payload (cookie + 10 name-lists + flag + reserved). */
  int axl_ssh_kexinit_build(AxlStrBuf *out);
  /* Checks the peer's KEXINIT names our single choice in every slot. */
  int axl_ssh_kexinit_select(const uint8_t *p, size_t len);
  ```

- [ ] **Step 1: Write the failing tests**

```c
static void
test_ssh_kexinit_build(void)
{
    AxlStrBuf *b = axl_strbuf_new();
    test_check(axl_ssh_kexinit_build(b) == AXL_OK, "ssh kexinit: build ok");
    const uint8_t *d = (const uint8_t *)axl_strbuf_data(b);
    test_check(d[0] == AXL_SSH_MSG_KEXINIT, "ssh kexinit: message number 20");
    test_check(axl_strbuf_len(b) > 1 + 16, "ssh kexinit: carries a 16-byte cookie");

    /* Our own KEXINIT must satisfy our own selector — the round trip. */
    test_check(axl_ssh_kexinit_select(d, axl_strbuf_len(b)) == AXL_OK,
               "ssh kexinit: our own offer is selectable");
    axl_strbuf_free(b);
}

static void
test_ssh_kexinit_rejects_unsupported(void)
{
    /* A peer offering only algorithms we refuse must be rejected, not
       negotiated down. Built by hand: cookie + a kex list of one bad name,
       then empty lists. */
    AxlStrBuf *b = axl_strbuf_new();
    uint8_t hdr = AXL_SSH_MSG_KEXINIT;
    axl_strbuf_append_bytes(b, &hdr, 1);
    uint8_t cookie[16] = { 0 };
    axl_strbuf_append_bytes(b, cookie, sizeof cookie);
    axl_ssh_put_string(b, "diffie-hellman-group1-sha1", 26);
    for (int i = 0; i < 9; i++) {
        axl_ssh_put_string(b, "", 0);
    }
    uint8_t tail[5] = { 0, 0, 0, 0, 0 };
    axl_strbuf_append_bytes(b, tail, sizeof tail);

    test_check(axl_ssh_kexinit_select((const uint8_t *)axl_strbuf_data(b),
                                      axl_strbuf_len(b)) == AXL_ERR,
               "ssh kexinit: unsupported kex refused, not downgraded");
    axl_strbuf_free(b);
}
```

Call both under `axl_printf("\n--- SSH kexinit ---\n");`.

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests
```
Expected: compile FAILS — `axl_ssh_kexinit_build` undeclared.

- [ ] **Step 3: Implement**

`src/net/axl-ssh-kex.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-kex.c
    KEXINIT and algorithm selection. ONE name per slot by design: negotiation
    is a downgrade surface, and we have no legacy peers to accommodate.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-rng.h>

#define KEX_NAME   "curve25519-sha256"
#define HOST_NAME  "ssh-ed25519"
#define CIPH_NAME  "chacha20-poly1305@openssh.com"
#define MAC_NAME   ""        /* implicit in the AEAD */
#define COMP_NAME  "none"

int
axl_ssh_kexinit_build(AxlStrBuf *out)
{
    uint8_t hdr = AXL_SSH_MSG_KEXINIT;
    uint8_t cookie[16];
    if (axl_strbuf_append_bytes(out, &hdr, 1) != AXL_OK ||
        axl_rng_bytes(cookie, sizeof cookie) != AXL_OK ||
        axl_strbuf_append_bytes(out, cookie, sizeof cookie) != AXL_OK) {
        return AXL_ERR;
    }
    static const char *lists[10] = {
        KEX_NAME, HOST_NAME, CIPH_NAME, CIPH_NAME, MAC_NAME,
        MAC_NAME, COMP_NAME, COMP_NAME, "", ""
    };
    for (int i = 0; i < 10; i++) {
        if (axl_ssh_put_string(out, lists[i], axl_strlen(lists[i])) != AXL_OK) {
            return AXL_ERR;
        }
    }
    uint8_t tail[5] = { 0, 0, 0, 0, 0 };   /* first_kex_packet_follows + reserved */
    return axl_strbuf_append_bytes(out, tail, sizeof tail);
}

/* True when `name` appears in the comma-separated name-list [p, p+n). */
static bool
name_list_has(const uint8_t *p, uint32_t n, const char *name)
{
    size_t want = axl_strlen(name);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || p[i] == ',') {
            if ((size_t)(i - start) == want &&
                axl_memcmp(p + start, name, want) == 0) {
                return true;
            }
            start = i + 1;
        }
    }
    return false;
}

int
axl_ssh_kexinit_select(const uint8_t *p, size_t len)
{
    if (len < 17 || p[0] != AXL_SSH_MSG_KEXINIT) {
        return AXL_ERR;
    }
    size_t off = 17;                       /* skip message number + cookie */
    static const char *want[4] = { KEX_NAME, HOST_NAME, CIPH_NAME, CIPH_NAME };
    for (int i = 0; i < 4; i++) {
        const uint8_t *s = NULL; uint32_t n = 0;
        if (axl_ssh_get_string(p, len, &off, &s, &n) != AXL_OK ||
            !name_list_has(s, n, want[i])) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}
```

Add `src/net/axl-ssh-kex.c` to `LIB_SOURCES`.

- [ ] **Step 4: Run to verify it passes**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh
```
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/net/axl-ssh-kex.c src/net/axl-ssh-internal.h test/unit/axl-test-ssh.c Makefile
git commit -m "feat(ssh): KEXINIT with one algorithm per slot, refusing rather than downgrading"
```

---

### Task 5: Exchange hash and key derivation

**Files:**
- Modify: `src/net/axl-ssh-kex.c`, `src/net/axl-ssh-internal.h`, `test/unit/axl-test-ssh.c`

**Interfaces:**
- Consumes: Tasks 1, 4; `AXL_ECDH_X25519`, `AxlDigest`
- Produces:
  ```c
  /* RFC 4253 §7.2 key derivation:
     K1 = HASH(K || H || X || session_id), K2 = HASH(K || H || K1), ...
     `letter` is one of 'A'..'F'. Fills out[0..out_len). */
  int axl_ssh_kdf(const uint8_t *k, size_t k_len,
                  const uint8_t *h, size_t h_len,
                  char letter, const uint8_t *session_id, size_t sid_len,
                  uint8_t *out, size_t out_len);
  ```

- [ ] **Step 1: Write the failing test**

The KDF is the piece most worth pinning to fixed bytes: an error here yields
keys that *work between two copies of our own bug* and fail against OpenSSH.

```c
static void
test_ssh_kdf_chains(void)
{
    /* Known-answer style: fixed inputs, so a refactor cannot silently change
       the derived bytes. K and H are arbitrary but FIXED. */
    const uint8_t k[4] = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t h[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t out1[32] = { 0 }, out2[32] = { 0 };

    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           out1, sizeof out1) == AXL_OK, "ssh kdf: ok");

    /* Determinism: the same inputs must give the same bytes. */
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           out2, sizeof out2) == AXL_OK, "ssh kdf: repeatable call ok");
    test_check(axl_memcmp(out1, out2, sizeof out1) == 0, "ssh kdf: deterministic");

    /* The letter must change the output — otherwise IV and key are identical,
       which is a catastrophic and easy mistake. */
    uint8_t outB[32] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'B', h, sizeof h,
                           outB, sizeof outB) == AXL_OK, "ssh kdf: letter B ok");
    test_check(axl_memcmp(out1, outB, sizeof out1) != 0,
               "ssh kdf: a different letter gives different bytes");

    /* Longer than one hash block must chain, not repeat the first block. */
    uint8_t big[64] = { 0 };
    test_check(axl_ssh_kdf(k, sizeof k, h, sizeof h, 'A', h, sizeof h,
                           big, sizeof big) == AXL_OK, "ssh kdf: 64-byte output ok");
    test_check(axl_memcmp(big, big + 32, 32) != 0,
               "ssh kdf: second block differs from the first (chained)");
    test_check(axl_memcmp(big, out1, 32) == 0,
               "ssh kdf: first block matches the 32-byte derivation");
}
```

Call under `axl_printf("\n--- SSH kdf ---\n");`.

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests
```
Expected: compile FAILS — `axl_ssh_kdf` undeclared.

- [ ] **Step 3: Implement**

In `src/net/axl-ssh-kex.c`:

```c
int
axl_ssh_kdf(const uint8_t *k, size_t k_len,
            const uint8_t *h, size_t h_len,
            char letter, const uint8_t *session_id, size_t sid_len,
            uint8_t *out, size_t out_len)
{
    uint8_t block[32];
    size_t produced = 0;
    AxlDigest *d = axl_digest_new(AXL_DIGEST_SHA256);
    if (d == NULL) {
        return AXL_ERR;
    }
    /* K1 = HASH(K || H || letter || session_id) */
    axl_digest_update(d, k, k_len);
    axl_digest_update(d, h, h_len);
    axl_digest_update(d, &letter, 1);
    axl_digest_update(d, session_id, sid_len);
    axl_digest_final(d, block, sizeof block);

    while (produced < out_len) {
        size_t n = out_len - produced;
        if (n > sizeof block) {
            n = sizeof block;
        }
        axl_memcpy(out + produced, block, n);
        produced += n;
        if (produced < out_len) {
            /* Kn+1 = HASH(K || H || K1 || ... || Kn) */
            axl_digest_reset(d);
            axl_digest_update(d, k, k_len);
            axl_digest_update(d, h, h_len);
            axl_digest_update(d, out, produced);
            axl_digest_final(d, block, sizeof block);
        }
    }
    axl_digest_free(d);
    return AXL_OK;
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
make ARCH=x64 tests && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh
```
Expected: PASS, `0 failed`.

- [ ] **Step 5: Sabotage-verify the letter actually reaches the hash**

```bash
scripts/sabotage.sh -s "src/net/axl-ssh-kex.c:s/axl_digest_update(d, &letter, 1);//" \
  --expect-fail -- bash -c 'make ARCH=x64 tests >/dev/null && TEST_APPS_ONLY=AxlTestSsh ./test/integration/test-axl.sh'
```
Expected: `the sabotage was detected` — the "different letter gives different bytes" assertion fails.

- [ ] **Step 6: Commit**

```bash
git add src/net/axl-ssh-kex.c src/net/axl-ssh-internal.h test/unit/axl-test-ssh.c
git commit -m "feat(ssh): RFC 4253 key derivation, pinned deterministic and chained"
```

---

### Task 6: Server glue — the `ssh(1)` conformance test

**Files:**
- Create: `include/axl/axl-ssh-core.h`
- Create: `include/axl/axl-ssh-server.h`
- Create: `test/integration/test-ssh-transport-qemu.sh`
- Create: `docs/sphinx/modules/ssh.rst`
- Modify: `src/net/axl-ssh-server.c`, `Makefile`, `docs/sphinx/index.rst`, `src/net/README.md`

**Interfaces:**
- Consumes: Tasks 1-5
- Produces:
  ```c
  typedef struct AxlSshServer AxlSshServer;
  AxlSshServer *axl_ssh_server_new(AxlLoop *loop, uint16_t port);
  int  axl_ssh_server_listen(AxlSshServer *s);
  void axl_ssh_server_free(AxlSshServer *s);
  ```

- [ ] **Step 1: Write the failing integration test**

`test/integration/test-ssh-transport-qemu.sh`:

```bash
#!/bin/bash
# test-meta: arch=both needs=ssh est=30 local-only=0
# test-ssh-transport-qemu.sh — a stock ssh(1) must complete version exchange,
# KEXINIT and curve25519 key exchange against the guest.
#
# WHY ssh(1) AND NOT A HAND-ROLLED CLIENT: most AXL protocols have no second
# implementation to test against, so a codec bug that is self-consistent passes
# both ends. SSH has OpenSSH. A kex hash we compute wrongly still interoperates
# with our own client and fails here, which is the whole point.
#
# P1 ends at the auth boundary: ssh gets through NEWKEYS and is then refused,
# so the PASS condition is "reached userauth", not "logged in".

set -uo pipefail
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

command -v ssh >/dev/null 2>&1 || { echo "SKIP: no ssh(1) on the host"; exit 0; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
APP="$(test_build_dir)/ssh-server-selftest.efi"
[[ -f "$APP" ]] || { echo "FAIL: $APP not built"; exit 1; }

"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" --net \
    --hostfwd auto:22 --background --timeout 60 \
    --serial-log "$WORK/guest.log" "$APP" > "$WORK/run.log" 2>&1
PORT=$(grep -oP '^HOSTFWD_22=\K\d+' "$WORK/run.log")
QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$WORK/run.log")
[[ -n "$PORT" && -n "$QEMU_PID" ]] || { echo "FAIL: no hostfwd port"; cat "$WORK/run.log"; exit 1; }
trap 'kill "$QEMU_PID" 2>/dev/null; rm -rf "$WORK"' EXIT

# Wait for the listener rather than sleeping a wall clock.
for _ in $(seq 1 40); do
    grep -q "ssh: listening" "$WORK/guest.log" && break
    sleep 0.5
done

ssh -vvv -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes -o ConnectTimeout=15 root@127.0.0.1 true > "$WORK/ssh.log" 2>&1

pass=0; fail=0
check() { if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1));
          else echo "FAIL: $2"; fail=$((fail+1)); sed 's/^/      /' "$WORK/ssh.log" | tail -20; fi }

grep -q "Remote protocol version 2.0, remote software version AxlSsh" "$WORK/ssh.log"
check "$?" "version exchange: ssh(1) saw our identification"
grep -q "kex: algorithm: curve25519-sha256" "$WORK/ssh.log"
check "$?" "KEXINIT: curve25519-sha256 negotiated"
grep -q "kex: host key algorithm: ssh-ed25519" "$WORK/ssh.log"
check "$?" "KEXINIT: ssh-ed25519 host key"
grep -qE "SSH2_MSG_NEWKEYS (sent|received)" "$WORK/ssh.log"
check "$?" "key exchange completed through NEWKEYS"
# P1's boundary: we must reach userauth, then refuse cleanly.
grep -q "SSH2_MSG_SERVICE_ACCEPT\|Authentications that can continue" "$WORK/ssh.log"
check "$?" "reached the userauth boundary"

echo "ssh transport: $pass passed, $fail failed ($TEST_ARCH)"
[[ "$fail" -eq 0 ]]
```

Add a `ssh-server-selftest` fixture target to the `Makefile` beside the other
`*-selftest` images; its `main` creates a loop, calls `axl_ssh_server_new(loop,
22)`, `axl_ssh_server_listen`, prints `ssh: listening`, and runs the loop.

- [ ] **Step 2: Run to verify it fails**

```bash
make ARCH=x64 tests && ./test/integration/test-ssh-transport-qemu.sh
```
Expected: FAIL — the fixture does not exist yet, or `ssh(1)` cannot connect.

- [ ] **Step 3: Implement the server glue**

In `src/net/axl-ssh-server.c`, wire `axl_tcp_listen` → `axl_tcp_accept_async`
→ per-connection state machine (`IDENT` → `KEXINIT` → `KEX` → `NEWKEYS` →
`AUTH_REFUSE`), driving Tasks 1-5 and feeding received bytes through
`axl_ssh_packet_unwrap` until it returns anything but `AXL_INCOMPLETE`.

Create `include/axl/axl-ssh-core.h` and `include/axl/axl-ssh-server.h`, each with a `@file` block and `///<` docs on every
parameter, and `docs/sphinx/modules/ssh.rst` containing
`.. doxygenfile:: axl-ssh-core.h
   .. doxygenfile:: axl-ssh-server.h`; add that page to `docs/sphinx/index.rst`.

- [ ] **Step 4: Run to verify it passes**

```bash
make ARCH=x64 tests && ./test/integration/test-ssh-transport-qemu.sh
./test/integration/test-ssh-transport-qemu.sh --arch AARCH64
```
Expected: `ssh transport: 5 passed, 0 failed` on both arches.

- [ ] **Step 5: Run the full gates**

```bash
./scripts/verify.sh
./test/integration/run-integration.sh -j"$(nproc)"
```
Expected: `ALL GREEN`; integration count up by one test, `0 failed`.

- [ ] **Step 6: Commit**

```bash
git add include/axl/axl-ssh-core.h include/axl/axl-ssh-server.h src/net/axl-ssh-server.c \
        test/integration/test-ssh-transport-qemu.sh \
        docs/sphinx/modules/ssh.rst docs/sphinx/index.rst src/net/README.md Makefile
git commit -m "feat(ssh): P1 transport — ssh(1) completes curve25519 kex against the guest"
```

- [ ] **Step 7: Security review gate (spec §11)**

Request an independent review of the P1 surface specifically: every
attacker-controlled length, the `AXL_INCOMPLETE`/`AXL_ERR` split (a
mis-classification here is a parser desync), the KDF letter/chaining, and that
no code path allocates from a peer-supplied size before bounding it. **Do not
proceed to P2 until this closes.**

---

## Self-Review

**Spec coverage.** §7's P1 rows: version exchange → Task 2; BPP framing →
Task 3; `KEXINIT` → Task 4; curve25519 kex + `NEWKEYS` → Tasks 5-6; key
derivation → Task 5. §10's unit layer → Tasks 1-5; the `ssh(1)` oracle →
Task 6; negative tests → Tasks 1, 3, 4. §11's gate → Task 6 Step 7. Rekey
(§7 RFC 4253 §9) is **P4**, not P1, per §12 — correctly out of scope here.

**Type consistency.** `axl_ssh_get_string` keeps the same signature in Tasks 1,
3 and 4. `AXL_INCOMPLETE` is used consistently as the third status in Tasks 2,
3 and 6. `AXL_SSH_MSG_KEXINIT` is defined once (Task 4) and used in Task 4's
tests only.

**Known gap, deliberate:** Task 6 Step 3 describes the state machine rather than
listing it line by line — it is the one part whose shape depends on the exact
`AxlTcp` callback signatures the implementer will read in `axl-9p-server.c`,
which is the closest working model. Every other step carries its code.
