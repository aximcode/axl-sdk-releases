# Axl9p — 9P2000.L filesystem protocol for AXL

**Status:** Design approved (2026-07-19). Ready for implementation planning.
**Author:** brainstormed with Mike.

## 1. Summary

Add native **9P2000.L** support to the AXL SDK so a UEFI host can both:

- **Serve** a UEFI volume over the network as a real filesystem that a Linux /
  WSL box mounts with `mount -t 9p -otrans=tcp` (kernel-native, no FUSE), and
- **Mount** a remote 9P share as a UEFI `fsN:` volume (via
  `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`), so the Shell and every UEFI app read/write
  it transparently.

All protocol logic lives in a new SDK library module `<axl/axl-9p.h>`; a single
first-class SDK tool `9p` (verbs `serve` / `mount` / `umount` / `serve-stop` /
`ls` / `get` / `put`) is a thin launcher over it, following the axl-webfs
resident-driver pattern.

## 2. Motivation

9P is dramatically simpler than SMB or NFS — Microsoft chose it for WSL for
exactly that reason (a few dozen size-prefixed little-endian message types, fid
handles, optional/no auth; no dialect negotiation, no RPC/XDR, no signing). A
feasibility **spike** (see §11) proved a working UEFI client — version-negotiate,
attach, walk, read a file, read a directory — in ~200 lines of C over the
existing sync `AxlTcp` API, byte-exact on both arches.

Use cases: expose a UEFI box's storage/logs/crash-dumps as a live mountable
drive for diagnostics (a technician `mount -t 9p`s it from a laptop); push test
payloads onto a UEFI ramdisk from Linux; pull dumps; edit firmware config files
from the host. The 9P *server* is the differentiated capability — axl-webfs
already does mount+serve over HTTP/WebDAV, but a native `mount -t 9p` requires
speaking the kernel protocol, which HTTP cannot.

## 3. Prior art

- **yabits/9pfsPkg** — an EDK2 UEFI 9P *client* publishing
  `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` over TCP, config via UEFI variables, aimed at
  network boot. We provide the same client capability but built on AXL's existing
  `AxlFsProvider` layer (no new SIMPLE_FILE_SYSTEM plumbing), programmatic (no
  UEFI-var/reboot dance), `.L`, and integrated so all AXL tools work over it. Its
  wire code (BSD-2-Clause-Patent, permissive) may be referenced.
- **axl-webfs** (sibling consumer repo) — the *architectural template*. It does
  `mount <url>` (workstation dir → `FSn:`) and `serve` (UEFI volumes over
  HTTP/WebDAV) as **resident embedded-DXE drivers** via `AxlService`. Axl9p mirrors
  its structure over the 9P wire protocol. Key files studied: `src/app/main.c`
  (verb tree), `src/app/cmd-mount.c` / `cmd-serve.c` (deploy pattern),
  `src/mount/webfs-mount.h` (opts + service descriptor + cross-binary ABI).
- **hugelgupf/p9** (u-root, Go) and **diod** (Linux) — reference `.L` servers we
  can test the client against.
- **Host 9P servers for tests** — the spike's `p9-spike-server.py`, grown into a
  small read-write server, or `diod`.

## 4. Non-goals (v1)

- **TLS.** Plain TCP v1 (LAN / QEMU / trusted diagnostics net). 9P-over-TLS is a
  clean v2 add (it is just a byte stream; `AxlTls` already exists).
- **virtio-9p transport.** TCP only. virtio is a possible v2 for QEMU dev.
- **9P2000.u / base 9P2000.** `.L` only, with a `Tversion` negotiate that fails
  loudly if the peer will not do `9P2000.L`. Base-version fallback is future work.
- **Authentication** (`Tauth`). None in v1 (transport-trusted). Future.
- **Windows-native / `\\wsl$` integration.** WSL's native transport is hvsocket,
  which UEFI cannot speak. Interop is "9P-over-TCP to any Linux (incl. a WSL
  distro running a TCP 9P server)". Documented as a known limit.

## 5. Architecture

```
                 9p tool (tools/9p.c, thin launcher: axl_args verb tree)
                              │
        one-shot verbs        │        resident verbs (AxlService, embedded DXE)
     ┌───────────────────┐    │    ┌──────────────────────────────────────────┐
     │ ls / get / put    │    │    │ serve  → 9p-serve-dxe.efi                  │
     │ (sync client)     │    │    │ mount  → 9p-mount-dxe.efi                  │
     └─────────┬─────────┘    │    └───────────────────┬──────────────────────┘
               │              │                        │
        ┌──────┴──────────────┴────────────────────────┴──────┐
        │              <axl/axl-9p.h>  (Axl9p)                 │
        │  ┌────────────┐  ┌───────────┐  ┌─────────────────┐  │
        │  │ wire codec │  │  client   │  │     server      │  │
        │  │ (encode/   │  │ (sync,    │  │ (async, per-conn│  │
        │  │  decode,   │  │  fid/tag) │  │  fid table)     │  │
        │  │  framer)   │  └─────┬─────┘  └────────┬────────┘  │
        │  └────────────┘        │                 │           │
        └────────────────────────┼─────────────────┼───────────┘
                        AxlFsProvider          AxlFs (fs0:\ backend)
                    (mount → fsN: volume)             │
                          AxlTcp (sync)          AxlLoop + AxlTcp (async)
```

Reuse, not reinvention:
- **Client mount** implements the `AxlFsProvider` vtable in terms of 9P and calls
  `axl_fs_provider_publish` → the SDK synthesizes `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
  on a fresh handle → the Shell sees a new `fsN:`. (Reference: `sdk/examples/memfs.c`.)
- **Server** maps incoming 9P requests onto `AxlFs` reads/writes/readdir against a
  real UEFI volume, on the async `AxlLoop`+`AxlTcp` machinery the HTTP server
  already uses. (Reference: `sdk/examples/http-server-driver.c`.)
- **Residency** for `serve`/`mount` uses `AxlService` + `AXL_EMBED` exactly as
  `service_demo` / axl-webfs do.

### AxlFsProvider ↔ 9P2000.L mapping (the mount bridge)

| AxlFsProvider callback | 9P2000.L |
|---|---|
| `open` (no CREATE)     | `Twalk` (clone to a new fid) + `Tlopen` |
| `open` (CREATE, file)  | `Twalk` parent + `Tlcreate` |
| `open` (CREATE, dir)   | `Twalk` parent + `Tmkdir` |
| `close`                | `Tclunk` |
| `read`                 | `Tread` (offset from provider-file state) |
| `read_dir`             | `Treaddir` (iterate the buffered entry list) |
| `write`                | `Twrite` |
| `seek`                 | update provider-file offset (9P read/write are offset-based) |
| `del`                  | `Tremove` (which also clunks the fid) |
| `flush`                | `Tfsync` (or no-op) |
| `get_info`             | `Tgetattr` |
| `set_info`             | `Trename` and/or `Tsetattr` |

Each open `AxlFsProviderFile` carries `{ fid, offset, is_dir, cached dir entries, path }`.

## 6. Library API (`<axl/axl-9p.h>`)

Types come from AXL's own conventions (standard C types, `AxlBytes`, `AxlArray`
of `AxlFsEntry`, `AxlHandle`, `AxlLoop`, `AxlStatus`). No EDK2 headers leak.

```c
// ============================ client =====================================
typedef struct Axl9pClient Axl9pClient;

/// Connect, negotiate 9P2000.L, and attach @p aname as the root.
/// @p uname may be "" ; @p aname defaults to "/" when NULL/empty.
int  axl_9p_connect(const char *host, uint16_t port,
                    const char *uname, const char *aname, Axl9pClient **out);
void axl_9p_disconnect(Axl9pClient *c);

/// One-shot file ops (chunk internally across msize-bounded Tread/Twrite).
int  axl_9p_read_file (Axl9pClient *c, const char *path, AxlBytes **out);
int  axl_9p_write_file(Axl9pClient *c, const char *path,
                       const void *buf, size_t len);       // creates/truncates
int  axl_9p_list      (Axl9pClient *c, const char *path, AxlArray **entries);
int  axl_9p_mkdir     (Axl9pClient *c, const char *path);
int  axl_9p_remove    (Axl9pClient *c, const char *path);
int  axl_9p_rename    (Axl9pClient *c, const char *from, const char *to);

/// Mount: publish the live connection as a UEFI fsN: volume.
/// Wraps axl_fs_provider_publish; the connection must outlive the mount
/// (owned by the resident mount driver — see §8).
int  axl_9p_mount  (Axl9pClient *c, bool read_only, AxlHandle *out_volume);
int  axl_9p_unmount(AxlHandle volume);

// ============================ server =====================================
typedef struct Axl9pServer Axl9pServer;

/// Serve an AxlFs subtree (@p root e.g. "fs0:\\" or a provider path) over 9P
/// on @p loop. Read-write unless @p read_only. One fid table per connection.
int  axl_9p_server_new   (AxlLoop *loop, const char *root,
                          bool read_only, Axl9pServer **out);
int  axl_9p_server_listen(Axl9pServer *s, uint16_t port);   // 0 = default 564
void axl_9p_server_free  (Axl9pServer *s);
```

The codec (message encode/decode, `qid`/`string`/`stat` helpers, and a
`recv-one-message` framer) is internal (`src/9p/axl-9p-codec.c`), shared by
client and server.

## 7. Wire protocol scope (9P2000.L)

Header `size[4] type[1] tag[2]`; `string = len[2] + utf8`; `qid = type[1]
version[4] path[8]`. Little-endian throughout. `Rlerror(type=7)` carries a Linux
`errno` on any failure.

Messages implemented:
`Tversion/Rversion`, `Tattach/Rattach`, `Twalk/Rwalk`, `Tlopen/Rlopen`,
`Tlcreate/Rlcreate`, `Tread/Rread`, `Twrite/Rwrite`, `Treaddir/Rreaddir`,
`Tmkdir/Rmkdir`, `Tgetattr/Rgetattr`, `Tsetattr/Rsetattr`, `Trename/Rrename` (or
`Trenameat`), `Tremove/Rremove`, `Tfsync/Rfsync`, `Tclunk/Rclunk`, `Rlerror`.

Client uses per-request tags (a small tag pool) though v1 is one-request-in-flight
per connection; server handles one tag at a time per connection. `msize`
negotiated (default 8 KiB, cap generous e.g. 64–128 KiB); reads/writes chunk to
`msize - header`.

## 8. The `9p` tool (`tools/9p.c`)

A first-class SDK tool (custom Makefile recipe, like `mkrd`; excluded from the
busybox multiplexer to keep it simple). Verb tree via `axl_args_run`, mirroring
axl-webfs `main.c`:

```
9p serve  <root> [--port 564] [--ro] [--nic N] [--listen-ip IP]   (resident)
9p serve-stop                                                     (unload serve)
9p mount  <host> [--port 564] [--aname /] [--ro] [--nic N] [--source-ip IP]  (resident)
9p umount                                                         (unload mount)
9p ls     <host>[:port] <path>                                    (one-shot)
9p get    <host>[:port] <path> [outfile]                          (one-shot)
9p put    <infile> <host>[:port] <path>                           (one-shot)
```

**Resident verbs (`serve`, `mount`)** follow the `AxlService` embedded-DXE
pattern:
- Single source compiled twice via `axl-cc --type service` (or the Makefile
  equivalent): once as `9p-serve-dxe.efi` / `9p-mount-dxe.efi` with
  `-DAXL_SERVICE_BUILD_DRIVER`, once embedded into `9p.efi` via `AXL_EMBED`.
- The launcher fills an opts struct (`Serve9pOpts` / `Mount9pOpts`, with an
  `AxlNetOpts net` member) and calls `axl_service_start_embedded(&deploy)`; opts
  cross the launcher→driver boundary via `AxlConfigDesc` descriptors serialized
  into LoadOptions.
- Driver `setup()`: `serve` builds `Axl9pServer` on the resident driver loop and
  listens (server pumped off the driver tick); `mount` connects + `axl_9p_mount`
  publishes the provider, then returns (provider calls are synchronous, no
  supervise loop). `teardown()`: `axl_9p_server_free` / `axl_9p_unmount` +
  disconnect.
- `serve-stop` / `umount` call `axl_service_stop(&deploy)` (resolves the driver
  by name-derived GUID, unloads it, runs `teardown`).

**One-shot verbs (`ls`/`get`/`put`)** are plain synchronous client calls, no
driver.

## 9. Error handling

- `Rlerror(errno)` → mapped to `AxlStatus` / `AxlFsStatus` (`ENOENT`→not-found,
  `EACCES`→access-denied, `EROFS`→write-protected, `EEXIST`, `ENOTDIR`, …).
- Connection drop mid-op: client fails the op and tears itself down; server reaps
  the connection (frees its fid table), like the HTTP server's `reset_connection`.
- `--ro` server answers writes/creates/removes with `Rlerror(EROFS)`.
- `Tversion` mismatch (peer will not do `9P2000.L`) → hard failure, no downgrade.
- Mount: a mid-session connection loss surfaces to the Shell as an I/O error on
  the affected file op; `umount` still tears the provider down cleanly.

## 10. Testing (test-first, both arches)

Bucket A (new public API) for the library; integration for the tool. Reuses the
spike's harness pattern.

- **Codec** — unit round-trips (encode→decode) for every message type; boundary
  cases (empty string, max `msize`, zero-length read). `AxlTestUtil` / a new
  `AxlTest9p` binary.
- **Client** — integration against a host read-write 9P server (grown-up
  `p9-spike-server.py` or `diod`) over QEMU user-net TCP: `connect`, `read_file`
  byte-exact, `list`, `write_file` round-trip, `mkdir`/`remove`/`rename`. Large
  file (> `msize`) to exercise chunked read/write.
- **Server** — the headline proof: boot UEFI `9p serve fs0:\`, then from the host
  **`mount -t 9p -otrans=tcp,port=N 127.0.0.1 /mnt`** and assert `ls` / `cat`
  (byte-exact) / `cp` (write) / `mkdir` / `rm` / `dd` (large) against the mount.
- **Mount** — boot UEFI `9p mount <host>`, then from the UEFI Shell assert
  `ls fsN:`, `hexdump fsN:\hello.txt` byte-exact, and a write-back
  (`cp fsN:\... `) reflected on the host.
- The feasibility spike (`run_9p_spike_mode`, `p9-spike-server.py`,
  `test-9p-spike-qemu.sh`) is **discarded** once the productized codec/client land
  — it is throwaway.

## 11. Spike findings (feasibility, already proven)

A throwaway spike ran a full UEFI→host round-trip over TCP, both arches (4/0
each): negotiated `9P2000.L`, walked+read `/hello.txt` byte-exact, listed a
directory. Confirmed: the client core is ~200 lines; the sync `AxlTcp` API is
sufficient (recv returns partial → a read-exact loop over the size-prefix);
encoding is trivial and portable (explicit little-endian ops → aa64 unchanged); a
host `.L` server is ~170 lines of Python. The spike code is not committed and
will be deleted; this design is TDD'd from scratch.

## 12. Phasing (each phase = its own implementation plan)

1. **Codec + client core** — encode/decode + framer; `axl_9p_connect`,
   `axl_9p_read_file`, `axl_9p_list`; codec unit tests + client integration.
2. **Client write path** — `axl_9p_write_file`, `mkdir`, `remove`, `rename`;
   chunked large read/write.
3. **Client `mount`** — `AxlFsProvider` bridge + `axl_9p_mount`/`_unmount`;
   `9p-mount-dxe.efi` resident driver; Shell-visible `fsN:` test. **DONE**
   2026-07-21 — bridge + public API + Shell-visible `fsN:` integration test
   (byte-exact read + write round-trip, both arches). The resident
   `9p-mount-dxe.efi` driver was descoped to Phase 5 per a 2026-07-20 scope
   decision (it belongs with that phase's opts-descriptor/launcher
   machinery, not the library-mount capability itself).
4. **Server** — `Axl9pServer` async on `AxlLoop`+`AxlTcp`, `AxlFs` backend,
   read+write, per-connection fid table; `9p-serve-dxe.efi`; Linux `mount -t 9p`
   integration test. **DONE** 2026-07-21 — public API
   (`axl_9p_server_new`/`_listen`/`_free`), transport + framing pump, a
   per-connection fid table, all 15 handlers (`Tversion` `Tattach` `Twalk`
   `Tclunk` `Tgetattr` `Tlopen` `Tread` `Treaddir` `Tlcreate` `Twrite`
   `Tmkdir` `Tremove` `Trename` `Tsetattr` `Tfsync`), a dispatch-level
   read-only gate, and a live-socket integration harness with an adversarial
   layer. 8219/0 unit and 231/0 9P-server integration on both arches.
   Descoped, as in Phase 3 and for the same reason: the resident
   `9p-serve-dxe.efi` driver moves to Phase 5 with the rest of the
   launcher/opts-descriptor machinery — the library capability is what
   Phase 4 owns.

   **Where reality deviated from this spec** (recorded, not quietly
   reworded):

   - §10's headline proof was a host **`mount -t 9p`** in CI. It is realized
     instead as an automated host **Python 9P2000.L client**
     (`test/integration/p9-client.py`) driving the guest server over a QEMU
     port forward. Reason: the kernel mount needs root and the `9p` /
     `9pnet_tcp` modules, neither of which the test harness has, so it would
     have been a locally-skipped test rather than a gate. The Python client
     speaks the identical wire and asserts byte-exact on every reply, and it
     reaches cases a mount cannot (malformed frames, pipelined frames,
     64-bit offsets, a full fid table). The kernel mount is documented in
     `src/9p/README.md` as the manual equivalent and is explicitly **not**
     claimed as tested.
   - **`Twalk` binding.** The Phase 4 plan's text had a partial walk bind
     `newfid`. Overruled by the project owner in favour of `walk(5)`:
     `newfid` is affected only when `nwqid == nwname`.
   - **`EXDEV` (18) and `EFBIG` (27) added to §9's errno vocabulary**, each
     deliberately. Cross-directory `Trename` answers `EXDEV` rather than
     taking `axl_file_move`'s unbounded whole-file copy; a `Tsetattr` grow or
     a `Twrite` reaching more than 16 MiB past EOF answers `EFBIG`. Both
     exist because growing/copying is O(size) *physical* synchronous I/O on
     the UEFI FAT driver and this server is one loop, so an unbounded one is
     a denial of service costing the client a couple of dozen wire bytes.
     Consequence at the time: our own `axl_9p_rename` had no `EXDEV`
     copy-then-unlink fallback, so a cross-directory rename from AXL's
     client against AXL's server returned `AXL_ERR` (a POSIX client degraded
     to copy-then-unlink instead). **Closed in Phase 5** (2026-07-22, by
     owner decision — see item 5's deviation list): `axl_9p_rename` now
     performs the bounded copy-then-unlink itself. Documented in
     `src/9p/README.md`.
   - **`Tclunk` may answer `Rlerror(EIO)`.** §9 did not anticipate an error
     reply to a clunk. A client that wrote and clunked without an explicit
     `Tfsync` has no other moment to learn its bytes never reached the
     media, so a failed final flush is reported rather than swallowed. The
     fid is released either way.
   - **`qid.path` is an FNV-1a hash of the export path**, not an inode:
     `AxlFs` exposes no inode or file index. It is therefore not stable
     across a rename. A v1 limitation, revisitable if `AxlFs` ever grows a
     real node identity.
   - **Times are a no-op**, as §7 allowed for v1, but the sharper statement
     is worth recording: the SDK has no mtime setter at all, so neither
     `Twrite` nor `Tsetattr(ATTR_SIZE)` bumps mtime — which is a divergence
     a Linux client can observe.
5. **`9p` launcher + build + docs** — verb tree, opts descriptors, Makefile
   recipe (twin-compile + embed), `TOOL_NAMES`/staging, README + Sphinx page.
   **DONE** 2026-07-22 — `tools/9p.efi`: the one-shot verbs (`ls`, `get`,
   `put`) over a synchronous `Axl9pClient` session, the resident pairs
   `serve`/`serve-stop` and `mount`/`umount` deploying the embedded
   `9p-serve-dxe.efi` / `9p-mount-dxe.efi` through `AxlService` (the two
   drivers Phases 3 and 4 descoped here), and a `status` verb. File split
   mirrors axl-webfs: `9p.c` verb tree, `9p-cmd-*.c` handlers,
   `9p-{serve,mount}-svc.{h,c}` dual-compiled services, `9p-common.{h,c}`
   the shared seam. Two integration suites on both arches —
   `test-9p-tool-qemu.sh` (one-shot verbs + `mount`/`umount`, the Shell
   reading *through* the published `fsN:`) and `test-9p-tool-serve-qemu.sh`
   (`serve --detach` driven by the host `p9-client.py`, plus a post-stop
   probe proving `serve-stop` closed the listener). Staged as `9p.efi` in
   `devkit.conf`; `README.md` tool table and `src/9p/README.md` updated.

   **Where reality deviated from this spec** (recorded, not quietly
   reworded):

   - **`--listen-ip` and `--source-ip` (§8's verb table) are not
     implemented.** No library API underneath accepts a bind address:
     `axl_9p_server_listen` takes a port and no local address, and
     `axl_9p_connect` has no source-IP parameter — even though
     `axl_tcp_connect_via` does, so the client half is plumbing rather than
     new capability. Shipping the flags would have meant widening both
     public signatures, which is a library change this phase did not own.
     `--nic` is the interface selector that does work, and it is the one
     that matters on firmware with a single usable NIC.
   - **`mount` never supervises; `serve` supervises unless `--detach`.**
     §8 described both as resident and said nothing about the foreground.
     Reality: `mount`'s whole point is that the `fsN:` is usable from the
     shell you typed the command into, so it always returns; `serve` holds
     the foreground until Ctrl-C by default, the way a server command
     usually behaves, and `--detach` opts out.
   - **A `status` verb was added**, not in §8's table. `serve-stop` /
     `umount` being idempotent makes "is it running?" a question a user
     will ask, and `axl_service_is_running` already answered it — one verb
     over two services rather than making people read `dh`.
   - **`9p` is not in `TOOL_NAMES`.** This item's own text said
     "`TOOL_NAMES`/staging"; §8 had it right ("excluded from the busybox
     multiplexer"). It has its own Makefile recipe because it links two
     embedded DXE driver blobs, which the multiplexer's one-object-per-tool
     rule cannot express. Consequence: `make tools` builds it explicitly,
     and it is absent from the release workflow's TOOL_NAMES sanity list by
     design (it still ships in the tools tarball, which globs `tools/*.efi`).
   - **The `EXDEV` copy-then-unlink fallback was pulled in from Phase 4's
     deferral list** by owner decision on 2026-07-22, rather than being left
     to v2 as item 4 above originally recorded. It is the phase's only
     library change: `axl_9p_rename` degrades to a bounded copy-then-unlink
     on `Rlerror(EXDEV)`, refusing directories, files over 32 MiB and an
     existing destination, and preserving neither mode, mtime nor ownership.
     See `src/9p/README.md`'s write-path section for the full contract.

## 13. Open items / future (v2+)

- 9P-over-TLS (wrap the byte stream in `AxlTls`).
- virtio-9p transport (QEMU dev convenience).
- Base 9P2000 / `.u` negotiation for non-Linux peers (u9fs, Plan 9).
- `Tauth` authentication.
- Read caching in the mount client (webfs has a cache layer;
  `src/mount/webfs-cache.c` is a reference if latency warrants it).
