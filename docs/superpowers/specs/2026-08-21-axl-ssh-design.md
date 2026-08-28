# AxlSsh — SSH for UEFI (server first, client later)

> **Status: PROPOSED 2026-08-21. Not started.** Owner of the facts about
> *why this is built rather than ported*, and about the scope that makes a
> hand-written SSH server defensible. ROADMAP carries the one-paragraph
> version; this is the argument.

## 1. Summary

An SSH implementation over `AxlTcp` + `AxlLoop`, shipped in axl-sdk and
consumed by SoftBMC the way it already consumes `AxlHttp` and `AxlTls`.
**The server ships first; the client is a later phase** — but the header
split is decided NOW, because retrofitting one after consumers exist is an
API break.

**Three headers, following `AxlHttp` rather than `Axl9p`:**

| header | contents |
|---|---|
| `<axl/axl-ssh-core.h>` | packet framing, `KEXINIT` codec, KDF, algorithm names — everything both roles use |
| `<axl/axl-ssh-server.h>` | `AxlSshServer`, command table, host key |
| `<axl/axl-ssh-client.h>` | *(later)* `AxlSshClient`, known-hosts, remote exec |

`AxlHttp` is the right model and `Axl9p` is not, for a reason specific to
this protocol: HTTP's halves genuinely diverge (`parse_request_line` vs
`parse_status_line`), whereas **SSH's transport is very nearly symmetric** —
identical packet framing, identical `KEXINIT` structure, identical key
derivation, identical AEAD. Only the roles differ: the server proves the host
key, the client signs the userauth request. So the shared core is most of the
implementation, and splitting it out is what makes the client cheap later.

Image size is not the reason for the split — `--gc-sections` is on every link,
so an app using only the server never pays for the client whatever the headers
look like. The reason is API clarity and not having to break it later.

It authenticates with **public keys only**, negotiates **one algorithm per
slot**, and exposes a **registered command table** rather than a pty or a
shell. It is not a general SSH server; it is a remote-command channel that
speaks enough of RFC 4251-4254 for a stock `ssh(1)` to connect.

## 2. Motivation

This project began as **TelCon/Netcon**, a *telnet* console server, and
SoftBMC grew from it into REST + console-mirror + RemoteTerminal. Telnet is
the one link in that chain with **no confidentiality and no authentication**.
SoftBMC's own remote-management design already reaches for `ssh` — but only
as a client-side `ssh -L` tunnel to its HTTPS API, which protects the
transport and leaves the console unprotected.

## 3. Prior art — and why none of it is portable here

Read against *our* constraints, not in the abstract. **Licence disqualifies
the best architectural fit before architecture is even discussed.**

| | licence | fits an Apache-2.0 SDK? | architecture fit |
|---|---|---|---|
| **wolfSSH** | **GPLv3** or commercial; requires wolfSSL/wolfCrypt on the same terms | **NO** | *best* — embedded-oriented, shell optional, no fork/pty requirement |
| **Dropbear** | MIT + permissive bundles (LibTom, TweetNaCl PD, 2-clause BSD) | yes | **bad** — `fork`/`forkpty`/`select` is its structure |
| **libssh** | LGPL-2.1 | poorly — static-only firmware linking makes the relink clause awkward | medium |
| **AxlSsh** | ours, Apache-2.0 | yes | native `AxlTcp`/`AxlLoop` |

axl-sdk is Apache-2.0 and **every dependency LINKED INTO our binaries is
permissive** — mbedtls Apache, libvterm MIT, freetype FTL/BSD-style, LZMA
public domain, stb and sdefl public-domain-or-MIT. wolfSSH would be the first
GPL code *linked* into an SDK that ships `.deb`/`.rpm`, and would relicense the
combined work.

The distinction is linking, not shipping: we already redistribute one GPL
binary, iPXE's `ipxe-all.efidrv`, in the tools tarball. That is sound precisely
because it is **mere aggregation** (GPL-2.0 §3) — a standalone driver next to
our tools, statically linked into nothing of ours, with the §3(b) written offer
carried as a pinned upstream commit. wolfSSH could not be handled that way; it
would be compiled into `libaxl.a`.

Dropbear's licence is fine and its *architecture* is the blocker, verified
against this tree rather than assumed:

- `fork` and `execve` appear **only in prose**; `src/service/README.md` states
  "UEFI has no `fork`/`setsid`/`chdir`/`umask`"
- no `sys/socket.h`, no `netinet/in.h`, no `pty.h` in the toolchain
- **no `select(2)` call site anywhere**; the `poll` hits are `AxlLoop`'s own
- `axl_socket_*` is object-based, not fd-based

Porting it means rewriting `svr-main` (accept/fork loop),
`svr-chansession` (`forkpty` + `exec`), the `select` session loop and auth —
most of the server — to land somewhere worse-fitting than our own primitives.

## 4. Non-goals (v1) — these ARE the security argument

Each of these removes attack surface, not just work.

- **No client in v1.** Server only *for now* — the client is P6 and reuses
  P1's core. This is a sequencing decision, not a permanent exclusion, which
  is why it is the one item here that is not a security argument.
- **No password or keyboard-interactive auth.** Public key only.
- **No pty, no shell, no arbitrary `exec`.** There is nothing to exec on UEFI.
  A session channel drives a **registered command table**; anything not in the
  table is refused.
- **No port forwarding** (`direct-tcpip`/`tcpip-forward`), **no agent
  forwarding**, **no X11**.
- **No file transfer — no SFTP subsystem and no `scp`.** See §4a; unlike the
  items above this one has a cheap path if it is ever wanted, so it is
  documented rather than merely excluded.

### 4a. What "supporting scp" would actually mean

Worth stating precisely, because the obvious answer is out of date.
**OpenSSH 9.0 (2022) switched `scp(1)` to the SFTP protocol by default** —
*"This release switches scp(1) from using the legacy scp/rcp protocol to using
the SFTP protocol by default"* — with `-O` to force the legacy protocol. So a
modern `scp` against AxlSsh speaks SFTP, not the old protocol, and "add scp"
splits into two very different projects:

| | what it is | cost | interoperates with |
|---|---|---|---|
| **legacy scp** | NOT a subsystem — an `exec` of `scp -t <path>` / `scp -f <path>`, then an ad-hoc protocol (`C0644 <size> <name>\n`, raw bytes, NUL, ack bytes 0/1/2) | small — a few hundred lines | `scp -O` only, plus pre-9.0 clients |
| **SFTP** | a `subsystem` channel request, ~30 message types, handle-based, with attrs and `realpath` semantics | large — comparable to all of `Axl9p` | modern `scp`, `sftp`, WinSCP, FileZilla |

**The legacy path fits this design better than it first appears.** It needs no
subsystem support and no new channel type: `scp` becomes an entry in the
registered command table (§8), so it does **not** weaken the "no arbitrary
exec" posture — the handler is registered by the application, not named by the
client. Requiring `-O` is a real usability wart, but a documented one.

**And file transfer may already be solved without SSH.** `Axl9p` ships a
working 9P2000.L **server** today (all five phases done 2026-07-22), so a host
can `mount -t 9p` the firmware's filesystem with no SSH involved; SoftBMC also
has an HTTPS API. If the requirement is "get files on and off the box", that
exists. If the requirement is "interoperate with the file-transfer tools people
already have", that is SFTP, and it is a phase of its own — see §12 P7.
- **No algorithm matrix.** One kex, one host-key type, one cipher (plus one
  documented fallback). Negotiation is a classic downgrade surface.
- **No compression.**

## 5. Architecture

```
  ssh(1)  ──TCP──▶  AxlTcp (async, EFI_TCP4)  ──▶  AxlSsh
                                                     ├─ transport: framing, kex, rekey
                                                     ├─ auth:      publickey only
                                                     └─ connection: one session channel
                                                            └─▶ AxlSshCommandFn table
```

**On `AxlTcp`, deliberately — not `AxlSocket`.** ROADMAP's networking-layering
item flags that `AxlSocket` is a BSD-compat veneer *alongside* the protocols,
with no library consumer, and that a socket-based server would make it
load-bearing and force that layering decision. HTTP and 9P build on `AxlTcp`
directly; AxlSsh does the same, so the layering item stays a deliberate
decision rather than something we trip into.

**Resident shape:** `AXL_SERVICE_DRIVER` over `AxlLoop`, like `Axl9pServer`.
Single-threaded, callback-driven, no blocking accept — a blocking `accept()`
in a resident driver freezes the firmware.

## 6. Algorithm set — already present in AxlCrypto

The hard, dangerous part is done and permissively licensed. These are exactly
what a default `ssh(1)` negotiates, so v1 interoperates with a stock client
with no `-o` flags:

| SSH name | AXL primitive |
|---|---|
| kex `curve25519-sha256` | `AXL_ECDH_X25519` |
| host key `ssh-ed25519` | `AXL_PK_ED25519` |
| cipher `chacha20-poly1305@openssh.com` | `AXL_AEAD_CHACHA20_POLY1305` |
| *(fallback)* `aes256-gcm@openssh.com` | `AXL_AEAD_AES_256_GCM` |
| hashing | `AxlDigest` (SHA-256) |

**We write framing and a state machine, not crypto.** The primitives are
mbedtls, audited and already vendored, not hand-rolled.

Host key persistence: `AxlNvstore`. Generated on first run if absent.

## 7. Wire protocol scope

| RFC | what we implement |
|---|---|
| 4253 §4.2 | version exchange (`SSH-2.0-AxlSsh_<ver>`) |
| 4253 §6 | binary packet protocol, AEAD mode only |
| 4253 §7-8 | `KEXINIT`, curve25519 kex, `NEWKEYS`, key derivation |
| 4253 §9 | **rekey** — required by ~1 GB / 1 h, not optional |
| 4252 | `userauth` publickey (with the `SSH_MSG_USERAUTH_REQUEST` signature check) |
| 4254 §5-6 | one `session` channel; `exec` mapped to the command table |

Explicitly refused with a clean disconnect: everything else.

## 8. Library API (`<axl/axl-ssh-server.h>`, sketch)

```c
typedef int (*AxlSshCommandFn)(AxlSshChannel *ch, int argc, char **argv,
                               void *user_data);

AxlSshServer *axl_ssh_server_new(AxlLoop *loop, uint16_t port);
int  axl_ssh_server_add_host_key(AxlSshServer *s, const char *nvstore_key);
int  axl_ssh_server_add_authorized_key(AxlSshServer *s,
                                       const char *openssh_pubkey_line);
int  axl_ssh_server_add_command(AxlSshServer *s, const char *name,
                                AxlSshCommandFn fn, void *user_data);
int  axl_ssh_server_listen(AxlSshServer *s);
void axl_ssh_server_free(AxlSshServer *s);

int  axl_ssh_channel_write(AxlSshChannel *ch, const void *buf, size_t len);
int  axl_ssh_channel_exit(AxlSshChannel *ch, int status);
```

## 9. Error handling

Every protocol violation ends the connection with `SSH_MSG_DISCONNECT` and a
reason code; no attempt to resynchronise. `AXL_ERR` on the API surface, with
the reason available for logging. A failed auth attempt costs the connection —
there is no retry counter to tune and no lockout state to keep.

## 10. Testing — OpenSSH is the conformance oracle

This is the part most AXL protocols never get: **a second, battle-tested
implementation to test against**, driven from the host.

`run-qemu.sh --net --hostfwd auto:22 --background` already does everything
needed; `axl-webfs` is driven exactly this way today.

1. **Unit (host, `test/unit/`)** — packet framing, kex hash construction,
   key derivation against RFC 4253 §7.2 vectors, `authorized_keys` parsing.
2. **Integration (`test/integration/test-ssh-*-qemu.sh`)** — real `ssh(1)`
   from the host: connect, authenticate by key, run a table command, read
   output, check exit status. `ssh -vvv` transcripts on failure.
3. **Negative** — wrong key refused; unknown command refused; unsupported
   algorithm refused with a clean disconnect; oversized packet refused.
4. **Rekey** — forced by a low byte threshold in a test build, then asserted
   with a long transfer under `ssh -o RekeyLimit`.
5. **Interop matrix** — the host's `ssh` is the oracle; record its version in
   the test log, because a client upgrade is a real source of drift.

## 11. Security review is a GATE, not a phase

Network-facing crypto protocol code, in firmware, before ExitBootServices,
with no OS isolation beneath it. A defect here is remote code execution in
firmware. The mitigations are structural, not procedural: §4's non-goals,
one algorithm per slot, publickey-only, no exec of anything not registered,
and audited primitives.

**No phase merges without an independent security review of that phase's
surface.**

## 12. Phasing (each phase = its own implementation plan)

| phase | deliverable | done when |
|---|---|---|
| **P1** | transport: version exchange, BPP framing, `KEXINIT`, curve25519 kex, `NEWKEYS` | `ssh -vvv` reaches "expecting SSH2_MSG_SERVICE_ACCEPT" then disconnects cleanly |
| **P2** | `userauth` publickey + `authorized_keys` | `ssh -i key` authenticates; wrong key refused |
| **P3** | session channel + command table + exit status | `ssh host cmd` returns output and status |
| **P4** | rekey, disconnect hygiene, `AxlNvstore` host key | rekey observed under `RekeyLimit`; key survives reboot |
| **P5** | `AXL_SERVICE_DRIVER` resident mode + SoftBMC consumption | runs as a driver; SoftBMC registers its own commands |
| **P6** | **client** (`<axl/axl-ssh-client.h>`) — reuses P1's core unchanged | `axl_ssh_client_exec` against OpenSSH `sshd`, with sshd as the oracle in the other direction |

| **P7** *(optional, unscheduled)* | file transfer — legacy `scp` via the command table, OR the SFTP subsystem | see §4a; pick ONE deliberately, they are different-sized projects |

P6 is deliberately last and deliberately cheap: if P1's core was split
correctly, the client adds a role, a known-hosts store and a userauth signer,
not a second protocol implementation. If P6 turns out to need core changes,
that is evidence P1 drew the boundary wrong.

P1 is the only phase that can be judged on its own — everything after it
depends on a working transport. Its plan is
`docs/superpowers/plans/2026-08-21-axl-ssh-p1-transport.md`.
