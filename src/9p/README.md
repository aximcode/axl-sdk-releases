9P2000.L for UEFI — both halves of the wire. A synchronous **client** over
`AxlTcp` (connect, read/write whole files, list/create/remove/rename, and
mount the connection as a UEFI `fsN:` volume), and an async **server**
(`Axl9pServer`) that exports an `AxlFs` subtree over TCP so a Linux host —
or another AXL client — can read and write the firmware's filesystem.

Header: `<axl/axl-9p.h>`. [9P](https://en.wikipedia.org/wiki/9P_(protocol))
is the Plan 9 file-protocol family; `9P2000.L` is the Linux-oriented dialect
(used by QEMU's `virtio-9p`/`Plan9FS`, WSL's `9p`, and `diod`) that this
module speaks. Client and server share one internal codec
(`src/9p/axl-9p-codec.c`) — message framing, strings, qids, cursors — so
neither side can drift from the other's idea of the wire.

Both halves also ship as a shell tool: `9p` (see "Tool" below) does
one-shot `ls`/`get`/`put`, and deploys the server or the mount as a
resident DXE driver so either outlives the command that started it.

## Client

A session is one TCP connection plus one attached tree root:
`axl_9p_connect` opens the socket, negotiates `9P2000.L` (`Tversion` /
`Rversion`, clamping to the server's `msize` if smaller), and attaches
`aname` as the root (`Tattach` / `Rattach`). Every subsequent call takes
the resulting `Axl9pClient *` and walks from that root — `axl_9p_read_file`
and `axl_9p_list` each do their own `Twalk` to the target path (fresh `fid`,
clunked when done), so paths are always absolute and independent of any
prior call.

```c
Axl9pClient *c = NULL;
if (axl_9p_connect("10.0.2.2", 564, NULL, NULL, &c) == AXL_OK) {
    AxlBytes *data = NULL;
    if (axl_9p_read_file(c, "/hello.txt", &data) == AXL_OK) {
        size_t n = 0;
        const void *p = axl_bytes_get_data(data, &n);
        axl_printf("%.*s\n", (int)n, (const char *)p);
        axl_bytes_unref(data);
    }
    axl_9p_disconnect(c);
}
```

`axl_9p_read_file` walks to the path, `Tlopen`s it read-only, and drains it
to EOF with chunked `Tread`s bounded by the negotiated `msize`, accumulating
into one `AxlBytes` the caller frees with `axl_bytes_unref`.
`axl_9p_list` walks to a directory, `Tlopen`s it, and pages through
`Treaddir` until the server reports zero remaining entries, projecting each
`qid`/`type`/`name` into an `AxlFsEntry` (skipping `.` and `..`) inside an
`AxlArray` the caller frees with `axl_array_free`. `Rreaddir`'s wire dirent
carries no size, so each entry the dirent's `dtype` did **not** mark as a
directory gets one extra `Tgetattr` round-trip to fill `AxlFsEntry.size`;
entries the dirent already marked as a directory keep 0 with no round-trip.
Both halves are the same field contract `axl_file_info` and `axl_dir_read`
publish ("file size in bytes, 0 for directories"), and the same one
`mount_get_info` enforces. `dtype` is not authoritative, though -- a server
can report `DT_UNKNOWN` for a real directory (ftype-less XFS, some NFS
re-exports behind diod/virtfs) -- so that same `Tgetattr` also reclassifies
the entry from `Rgetattr`'s `st_mode` and re-zeros its size when the mode
says directory, closing the gap dtype alone would leave open. A stat that
fails for one entry (e.g. a concurrent remove) leaves that entry's size at
0 and does not fail the listing; three consecutive failures are read as a
dead session and fail the call with `AXL_ERR`, rather than returning a
listing whose sizes are silently wrong. Only failures that reached the
*server* count toward those three — an entry whose name makes the child
path too long to build locally is skipped identically but is no evidence
about the connection, so a run of long names cannot trip a dead-session
verdict on a healthy session.

### Write path

Four functions round out the client with mutation over the same
connect/walk pattern:

- `axl_9p_write_file` — `Twalk` to the path; if it exists, `Tlopen`s it
  write-only and truncates it, otherwise `Twalk`s to the parent and
  `Tlcreate`s the file there. Writes the buffer with chunked `Twrite`s
  bounded by `msize`, then clunks the fid.
- `axl_9p_mkdir` — `Twalk`s to the parent and `Tmkdir`s the final path
  component (mode 0755).
- `axl_9p_remove` — `Twalk`s to the path and issues `Tremove`.
- `axl_9p_rename` — `Twalk`s to the source and to the destination's
  parent, then `Trename`s the source into that parent under the
  destination's final component. On `Rlerror(EXDEV)` it degrades to a
  bounded copy-then-unlink (see the gotcha below).

Three gotchas the code encodes and the tests pin down:

- `axl_9p_write_file` is **truncate-or-create, not recursive**: it will
  truncate an existing file or create a new one, but the parent directory
  must already exist — call `axl_9p_mkdir` first if it might not.
- `axl_9p_remove` uses `Tremove`, which clunks the fid server-side as
  part of removing it (on both success and error). The client does
  **not** send a separate `Tclunk` afterward — doing so would be a
  double-clunk against a fid the server has already retired.
- `axl_9p_rename` issues a `Trename` and, if the server refuses it with
  `Rlerror(EXDEV)` — which `Axl9pServer` does for every cross-directory
  rename, see "Rename is same-directory only" below — **falls back to
  copy-then-unlink on the client**, the same degradation `mv(1)` and every
  other `rename(2)` caller performs. The fallback is deliberately bounded
  and its partial-failure modes are visible rather than papered over:
  a **directory** is refused with `AXL_ERR` (a recursive tree copy has
  partial-failure semantics `rename()` does not have); a file **larger
  than 32 MiB** is refused with `AXL_ERR` (the whole file is materialized
  in UEFI heap, peak ~2x the file size); and if the copy succeeds but the
  source cannot be removed, **both paths now exist** — the destination is
  left in place and `AXL_ERR` is returned, because the copy happened and
  the move did not. Only a rename that both copied and unlinked returns
  `AXL_OK`. Unlike `rename(2)`, the fallback **refuses an existing
  destination** rather than overwriting it — `rename(2)`'s permission to
  clobber is only safe because the replacement is atomic, and a
  copy-then-unlink is not (a session drop mid-copy would leave the
  destination corrupted rather than either whole file) — matching the
  same-directory path against `Axl9pServer`, which refuses a taken
  destination with `EEXIST` too. A caller that wants replace semantics
  removes the destination first. The copy also does **not preserve mode,
  mtime, or ownership**: the write path creates the destination at mode
  0644 with a fresh mtime under the attach user, inert against AXL's own
  FAT-backed server but not against a POSIX-backed one (diod,
  nfs-ganesha, kernel exportfs).

### Mount

`axl_9p_mount(c, read_only, &token)` bridges a connected `Axl9pClient`
onto `<axl/axl-fs-provider.h>` and publishes it as a UEFI `fsN:`
volume — Shell, LoadImage, and the Boot Manager see a normal
filesystem, transparently backed by 9P `Twalk`/`Tlopen`/`Tread`/
`Twrite`/etc. Each open UEFI file handle holds its own persistent 9P
`fid` (walked once on `Open`, `Tclunk`'d on `Close`); reads/writes are
offset-based `Tread`/`Twrite` chunked to the negotiated `msize`;
directories page through `Treaddir` into a buffered entry list.

```c
Axl9pClient *c = NULL;
void *vol = NULL;
if (axl_9p_connect("10.0.2.2", 564, NULL, NULL, &c) == AXL_OK
        && axl_9p_mount(c, false, &vol) == AXL_OK) {
    /* fsN: is now Shell-visible; the caller can dir/type/copy it. */
    axl_9p_unmount(vol);        /* before disconnect */
    axl_9p_disconnect(c);
}
```

Two contracts to keep straight:

- **The connection is borrowed, not owned.** `c` must outlive the
  mount — call `axl_9p_unmount` before `axl_9p_disconnect`;
  disconnecting first leaves the mount's fids pointing at a dead
  socket.
- **`read_only` is enforced in the bridge itself**, not just
  documented: passing `true` wires `write`/`del`/`set_info` out of the
  `AxlFsProvider` vtable entirely (`NULL` callbacks, which the
  provider thunk maps to `EFI_WRITE_PROTECTED`/`EFI_UNSUPPORTED`), and
  `Open` rejects `AXL_FS_OPEN_WRITE`/`AXL_FS_OPEN_CREATE` up front — a
  read-only mount can't be coaxed into a write via some other
  `EFI_FILE_PROTOCOL` entry point.

The `fsN:` shell-mapping assignment on publish (and its best-effort
fallback when no shell is present, e.g. a DXE driver) is
`axl_fs_provider_publish`'s own behavior — see `src/fs/README.md`'s
`AxlFsProvider` section.

## Server

`Axl9pServer` exports an `AxlFs` subtree over TCP. It is async and runs on
the **caller's** `AxlLoop` — there is no thread and no internal pump; a
server is serviced by whatever already drives that loop (an app's main
loop, an `AxlService` driver tick).

```c
Axl9pServer *s = NULL;
if (axl_9p_server_new(loop, "fs0:\\9pexport", false, &s) == AXL_OK
        && axl_9p_server_listen(s, 5640) == AXL_OK) {
    /* pump `loop` for as long as you want to serve */
    axl_9p_server_free(s);      /* reaps every live connection */
}
```

- `axl_9p_server_new(loop, root, read_only, &out)` — `root` is an `AxlFs`
  path prefix (`"fs0:\\"`, a RAM-disk volume, a provider-backed volume)
  that every client path resolves under. A trailing separator is stripped;
  the stored root is capped at 256 bytes. `read_only = true` answers every
  mutating message `Rlerror(EROFS)` — see the gate below.
- `axl_9p_server_listen(s, port)` — begins accepting. `port == 0` selects
  the registered 9P port, 564. On failure the port is released before the
  call returns, so a retry is immediately possible.
- `axl_9p_server_free(s)` — stops the listener, reaps every live
  connection (releasing whatever fids each held open), and frees the
  server. `NULL`-safe.

  The teardown is **abortive and port-releasing**, deliberately, with no
  mode to choose: freeing the server has no "keep serving" variant, and its
  callers go on to free the `AxlLoop` it ran on. The listener and every live
  connection are closed with a TCP RST and finalized *synchronously and
  loop-free* — including the firmware accept backlog and any already-deferred
  closes of connections accepted from this listener — so the listen port is
  free on return and no close source is left registered on the loop. That is
  what makes `serve` → `serve-stop` → `serve` on the same port repeatable
  within one boot: EFI_TCP4 has no `SO_REUSEADDR`, so a deferred close
  strands the port for the rest of the boot and the next `Configure()` fails
  with `EFI_INVALID_PARAMETER`. The trade is the usual one — the RST discards
  any un-ACKed tail of an in-flight reply, and clients see a reset rather
  than a clean FIN. Connections dropped while the server is *running* are
  unaffected: those still get a graceful FIN.

### Messages implemented

`Tversion` `Tattach` `Twalk` `Tclunk` `Tgetattr` `Tlopen` `Tread`
`Treaddir` `Tlcreate` `Twrite` `Tmkdir` `Tremove` `Trename` `Tsetattr`
`Tfsync` — plus `Rlerror`, which carries a Linux `errno` on every failure.
Anything else on the wire (`Tauth`, `Tsymlink`, `Tlink`, `Tstatfs`, the
`Txattr*` family) is answered `Rlerror(EPROTO)`.

The **read-only gate** is one predicate over the message type, applied in
`s9p_dispatch` before any handler runs, so a read-only export cannot be
mutated through a handler that forgot to check: `Tlcreate`, `Twrite`,
`Tmkdir`, `Tremove`, `Trename` and `Tsetattr` are refused `EROFS`. The
integration suite drives every one of those six over a genuinely read-only
export rather than trusting the list.

`Tfsync` is deliberately **not** in that set: it flushes only state the
server already accepted, and POSIX permits `fsync` on a read-only
descriptor, so an OPEN fid with no write stream behind it (a reader, a
directory) is answered `Rfsync` without touching anything. It does apply
the same open-state gate its siblings do: an unbound fid, and one walked
to but never `Tlopen`'d, are both `EBADF` — `fsync(2)` has no meaning
without an open file description, and `Tread`, `Treaddir` and `Twrite` all
refuse the same fid.

### Sizing

| Knob | Value | Why |
|---|---|---|
| Connection pool | 8, fixed | the `S9pConn` slot array is preallocated, so the CONNECTION COUNT is bounded with no allocation. Each accept still allocates that slot's `rbuf` and `txbuf` (`msize` bytes each, 8 KiB at the default), and the reap frees them |
| Fids per connection | 128 | bounds worst-case firmware handles |
| `msize` | 8 KiB default, negotiated up to 128 KiB | `rbuf`/`txbuf` grow to the negotiated value and never shrink |
| `msize` floor | 512 | below it a session cannot carry a useful request; `Tversion` refuses with `Rversion(..., "unknown")` |
| Page cache | 64 KiB × 16 frames = 1 MiB, **server-wide** | one shared `AxlPageCache` for every open file fid on every connection — per-fid pools would let 8 conns × 128 fids multiply the budget a thousandfold |

### v1 limits and deliberate deviations

These are decisions, not omissions. Each one is here because the
alternative was worse on this platform.

**`qid.path` is a hash of the path, not an inode.** It is FNV-1a over the
export path string; `qid.version` is always 0. Consequence: a rename
changes a node's `qid.path`, so two qids for "the same" file across a
rename do not match, and in principle two distinct paths could collide.
`AxlFs` exposes no inode or file index to source a stable identity from; a
future revision can switch to one if it appears. Paths are what fids and
clients actually key off in a single-server session, so this is cheap in
practice.

**One request in flight per connection.** The server handles one tag at a
time. Pipelined requests arriving in a single TCP segment are drained in
order, not concurrently — they are *safe* (the integration suite fires the
exact pipelined shapes that once caused two heap use-after-frees), just not
parallel.

**A partial `Twalk` does not bind `newfid`.** This follows `walk(5)`
exactly: `newfid` is affected only by a walk that completed every component
(`nwqid == nwname`). A partial walk is not a wire error — the server
returns the qids it did reach — but it leaves `newfid` unbound, so a client
must not assume it can clunk it. (An earlier draft bound it on partial
success; that was corrected to the spec's behavior.)

**Grow is bounded at 16 MiB per request; shrink is unbounded.**
`Tsetattr(size)` may grow a file by at most 16 MiB beyond its current
length, and `Twrite` may reach at most 16 MiB past EOF; over that, both
answer `Rlerror(EFBIG)`. The reason is not a storage limit, it is a
scheduling one: growing a file on UEFI is not a metadata update. The
EDK2-derived FAT driver physically writes the added region out as zeros,
UEFI file I/O is synchronous, and this server is a **single loop** — so a
grow services nothing else, no other connection, no accept, no timer, for
its entire duration. A `Tsetattr(size = 4 GiB)` is a 67-byte message and
minutes of dead loop; a `Twrite(offset = 0xFFFFFFFE, count = 1)` is
twenty-four bytes for the same stall. 16 MiB is the largest single stall
worth accepting (a few seconds on slow USB media). A client that genuinely
wants a bigger file is not blocked — it is made to ask in bounded steps,
and the loop returns between them, which is the property that matters.
Shrinks move no data, so they are not bounded. No free-space check
accompanies the ceiling: `AxlFs` has no volume-free-space API, so a grow
that fits the bound but not the volume still fails, as `Rlerror(EIO)`.

**Rename is same-directory only; cross-directory answers `EXDEV`.**
`axl_file_rename` refuses a cross-directory rename outright (most UEFI FAT
drivers cannot move a node between directories via `SetFileInfo`), and the
only fallback — `axl_file_move` — is an unbounded, synchronous whole-file
copy: the same single-loop stall the grow ceiling exists to prevent, except
a rename has no natural ceiling to apply. `EXDEV` is chosen because it is
exactly what a `rename(2)` caller already knows how to handle: `mv(1)`,
coreutils and the Linux 9p client all fall back to copy-then-unlink on it,
which comes back as ordinary bounded `Twrite`s the loop can interleave. So
this **degrades** rather than breaking — for a POSIX client and, since the
`EXDEV` fallback landed, for our own `axl_9p_rename` too (see the client's
write-path section above for the fallback's bounds and its
both-paths-exist failure mode).

A renamed fid is also left **bound but closed** — its open handles are
dropped before the rename and are not reopened after, so it must be
`Tlopen`'d again. Not because the firmware refuses a rename with a live
handle: it does not, and the suite pins that (`CASE43` asserts an
`Rrename` while *another connection* holds the same file open). The reason
is that a handle surviving the rename keeps naming the moved node while
the fid's stored path names where the node used to be, and a fid that
refers to two different things is worse than one that must be reopened.
Renaming a file you are actively writing is unusual; renaming a closed
one, which is every `mv(1)`, is unaffected.

**Directory rename leaves descendant fids naming the old path.** `Trename`
has no file-only restriction — a client may rename a directory, and the
server allows it. Only the fid named in the request has its stored path
rewritten. A fid bound to something *underneath* the renamed directory
still carries its pre-rename path, which no longer resolves: the next
operation on it fails (`ENOENT`, or `EIO` on a read through an already-open
view). Rewriting every descendant fid on every connection would mean a
prefix scan of 8 × 128 stored paths per rename, and the fid is a client-side
handle the client knows it renamed under. Clunk and re-walk after renaming a
directory you hold fids beneath.

**`Tclunk` reports a failed final flush as `Rlerror(EIO)`.** A client that
wrote and clunked without an explicit `Tfsync` has no other moment to learn
its bytes never reached the media, so the server flushes the fid's write
stream explicitly before closing it and turns a flush failure into `EIO`
rather than a mute `Rclunk`. The fid is released either way — 9P clunks it
even when the reply is an error. What this does and does not promise: the
server reports the status the firmware's flush
(`EFI_FILE_PROTOCOL.Flush` / the Shell's `FlushFile`) returns to it. A
successful `Rclunk` or `Rfsync` means the write stream was drained and the
firmware accepted the flush; it is not a claim about what the device did
below that. Closing alone is *not* a durability point in AXL and is not
treated as one here — see `src/stream/README.md`.

**Times are accepted and discarded.** `Tsetattr` honors `ATTR_SIZE` only.
`mode`/`uid`/`gid` are no-ops because `AxlFs` has no POSIX permission or
ownership model, and `atime`/`mtime` are no-ops because the SDK exposes no
mtime setter at all. Concretely: `Tsetattr(ATTR_MTIME)` cannot set an
mtime, because there is nothing in `AxlFs` to set one with — a Linux client
that issues `utimensat(2)` gets `Rsetattr` and no change. Whether the
underlying FAT driver bumps mtime **on its own** when a `Twrite` or a
size change lands is firmware behavior that nothing in this tree pins or
promises, so do not read this as "mtime never moves". `Rsetattr` carries no
field asserting what was set, so the reply is not lying — but a client
relying on the server to maintain mtime should not.

**The export is close-to-open consistent, not coherent.** This is the
`AxlFileView` consistency model (see `src/fs/README.md`) reaching the wire,
and it is the same guarantee NFS gives by default — which is a useful
coincidence, since the clients most likely to mount this are Linux v9fs
clients that already reason that way.

- **Guaranteed:** a `Tlopen` sees the file's current contents. The open
  stats the file itself, so this holds against every writer — another
  image, the Shell, the firmware. A client that wants current data should
  re-open the fid.
- **Not guaranteed:** that a fid already open when the file changes will
  see the change. As a best effort it usually does, when the write went
  through AXL in this PE image — a write through the same fid, another
  fid, another connection, a *second* `Axl9pServer` exporting the same
  root, or any non-9P AXL code in the image. The integration suite's
  `CASE44` drives the two-server case end to end, and it matters because
  Linux v9fs over `trans=tcp` defaults to `cache=none`, so every client
  read reaches the server rather than a client-side cache.

A writer in a **different EFI image** is outside the best effort entirely.
The boundary is the PE image, not the process or the machine: `libaxl` is
statically linked, so a launcher and a driver it loads are two images with
two independent registries and neither can see the other's — as are the
UEFI Shell, another application, and the firmware itself. UEFI has no
file-change notification to bridge them, and the server does not stat per
read (that per-operation firmware round trip is exactly what the page cache
exists to avoid).

So: do not export a tree that something outside this image is writing and
expect an open fid to see it. Re-opening is the contract, and it works.

**Single-tenant attach.** Every `Tattach` binds its fid to the whole export
root; `afid`, `uname`, `aname` and `n_uname` are read to advance the wire
cursor and otherwise ignored. There is one tree and no authentication.
`Tversion`'s tag is not required to be `NOTAG` — the server echoes whatever
tag arrived.

**Volume-full and I/O error are indistinguishable.** `AxlFs` collapses both
into `AXL_ERR`, so both surface as `Rlerror(EIO)`. Where a distinction is
recoverable — missing path, is-a-directory, write-protected — the handlers
pre-call `axl_file_info` to recover it and answer `ENOENT` / `EISDIR` /
`EROFS` properly.

### Escaping the export root

A client cannot walk out of the exported subtree. Every `Twalk` component
is validated as one simple path segment before use: non-empty, not
truncated against the buffer it was copied into, no embedded `NUL`, and
free of **both** separators `AxlFs` recognizes — `/` *and* `\`. Rejecting
only `/` would leave a hole, since a component like `..\..\secret` contains
no `/` but `axl_path_join` would still treat the embedded `\` as a real
separator. `..` is handled by `s9p_path_up`, which refuses to ascend past
the root, and a fid's path is only ever extended through
`s9p_path_append`/`s9p_path_up` — so "inside the root" is an invariant of
those two functions rather than a duty of each handler.

### Mounting it from Linux

```
mount -t 9p -otrans=tcp,port=<n>,version=9p2000.L,msize=8192 <ip> /mnt
```

This needs root and the kernel `9p` / `9pnet_tcp` modules, neither of which
the automated harness has, so it is **manual / real-hardware validation and
has not been run**. What *has* been run, on both arches, is a host-side
Python 9P2000.L client driving the identical wire over a QEMU port forward
— same messages, same framing, same replies, asserted byte-exact. That is
evidence the kernel mount should work; it is not a claim that it has been
tested.

## Tool

`tools/9p.c` is the shell front end over everything above — `9p.efi`, a
first-class tool with its own Makefile recipe rather than a busybox
subcommand, because it links two embedded DXE driver images and the
multiplexer's one-object-per-tool rule cannot express that.

```
9p ls     <host>[:port] [path]              one-shot
9p get    <host>[:port] <path> [outfile]    one-shot
9p put    <infile> <host>[:port] <path>     one-shot
9p serve  [root] [--ro] [--detach]          resident (9p-serve-dxe.efi)
9p serve-stop                               unload the server
9p mount  <host>[:port] [--aname /] [--ro]  resident (9p-mount-dxe.efi)
9p umount                                   unload the mount
9p status                                   which services are resident
```

Every verb that names a server — `ls`, `get`, `put`, `mount` — takes it as
`host` or `host:port`, through one shared splitter
(`axl9p_split_host_port`, `tools/9p-common.c`); the port defaults to the 9P
well-known **564**. `--nic N` selects which NIC to bring online (default:
the first usable one), and the two resident verbs also take `--port` for
the port they serve on / connect to. Where both apply — `mount` — an
inline `:port` **wins** over `--port`: the more specific form takes
precedence, and `--port`'s own default supplies 564 when neither names one.

**One-shot (`ls` / `get` / `put`)** — a synchronous `Axl9pClient` session
per invocation: connect, do the one thing, disconnect. Nothing is left
behind. `ls` prints one `d|f <size> <name>` line per entry (a directory's
size is 0, per the field contract above). `get` with no `outfile` streams
the file to stdout byte-exact — no added terminator, binary-safe — and
with an `outfile` writes it locally, including the empty-file case.

**Resident (`serve` / `mount`)** — these deploy an embedded DXE driver
through `AxlService`: `axl_service_start_embedded` loads
`9p-serve-dxe.efi` / `9p-mount-dxe.efi` out of the launcher's own image,
and the driver's options cross the launcher→driver boundary as
`AxlConfigDesc`-described `LoadOptions`. The driver is what stays alive,
so the export (or the `fsN:` volume) outlives the command; `serve-stop` /
`umount` unload the image, running its teardown.

The two verbs deliberately differ in what they do *after* the deploy:

- **`serve` supervises in the foreground until Ctrl-C**, so the shell
  shows a running export the way a server command usually behaves.
  `--detach` starts the driver and returns to the prompt instead; the
  export keeps running either way, and only `serve-stop` ends it.
- **`mount` always returns.** Residency is the entire point of the verb —
  the volume must be usable from the shell you typed the command into —
  so there is nothing to supervise. It prints the `fsN:` name it
  published, resolved by diffing the volume list around the deploy.

Re-running `serve` or `mount` while the matching driver is already
resident does **not** reconfigure it — the running instance keeps the
options it was deployed with. The verb says so and succeeds rather than
silently implying the new flags took effect; stop it and start it again
to change anything. `serve-stop` and `umount` are idempotent and report
"not serving" / "not mounted" rather than failing.

**`--listen-ip` and `--source-ip` (design spec §8) are not implemented.**
Not an oversight in the tool: no library API underneath accepts a bind
address. `axl_9p_server_listen` takes a port and no local address, and
`axl_9p_connect` has no source-IP parameter — even though
`axl_tcp_connect_via` does, so the client-side half is a plumbing job
rather than a new capability. `--nic` is the interface selector that
*does* work, and on a firmware with one usable NIC it is the only one
that would have mattered anyway. Adding the flags would mean widening
both library signatures first.

The tool exposes no `rename`, `mkdir` or `rm` verb — `put` is its only
mutation. The rest of the write path is library surface
(`axl_9p_mkdir`, `axl_9p_remove`, `axl_9p_rename` and its `EXDEV`
copy-then-unlink fallback), documented under "Write path" above.

## Validation

- **Codec** — `test/unit/axl-test-9p.c` (`AxlTest9p`): message framing,
  strings, cursors, and the pure wire-arithmetic bounds helpers
  (`s9p_write_grow_ok`, `s9p_readdir_resume`, …), both arches.
- **Client + mount** — `test/integration/test-9p-qemu.sh`: a UEFI guest
  connects out to a host Python 9P2000.L server (`p9-server.py`) — read
  byte-exact, list, write/read-back, truncate-existing, chunked
  multi-`msize` round-trip, mkdir, remove, rename, and a cross-directory
  rename the fixture refuses with `Rlerror(EXDEV)` so the client's
  copy-then-unlink fallback is exercised against the same answer
  `Axl9pServer` gives — then `9p-mount-selftest.efi` mounts the same
  server as a UEFI `fsN:` volume and proves the mount path end to end.
  (Both arches, by its `test-meta` line.)
- **Server** — `test/integration/test-9p-server-qemu.sh`, both arches: the
  inverse topology. A guest app (`9p-server-selftest.efi`) seeds a tree, exports it
  with `axl_9p_server_new` + `axl_9p_server_listen`, and pumps the loop;
  QEMU forwards a host port to it and `p9-client.py` drives the whole
  functional surface plus an adversarial layer — malformed and **pipelined**
  frames, counts that lie about the frame, 64-bit offsets and dirent
  cursors, a full fid table, `EFBIG`, `EXDEV`, and the `EROFS` gate on every
  mutating message against a second read-only export. Assertions are the
  host client's own stdout matched exactly, never a grep of the guest's log
  — the server is never graded on its own testimony.
- **Tool, client side** — `test/integration/test-9p-tool-qemu.sh`, both
  arches: the same guest-connects-out topology as the client suite, but the
  guest runs the shipped `9p.efi` from the Shell, so what is under test is
  the launcher — argv parsing, `host[:port]` splitting, NIC bring-up, and
  the printed lines a human reads. `ls`, `get` to stdout, `get` to a file
  with a Shell `type` read-back, `put` with a `get` read-back, then the
  resident `mount` / `umount` pair: the Shell reads a file *through* the
  published `fsN:`, which is the claim that the mount outlived the command,
  and repeats the same read after `umount` to prove the provider stopped
  answering rather than the unload merely having looked clean.
- **Tool, server side** — `test/integration/test-9p-tool-serve-qemu.sh`,
  both arches: `9p.efi serve … --detach` deploys the embedded
  `9p-serve-dxe.efi`, and `p9-client.py` drives it over a QEMU port
  forward. What this owns beyond the server suite is the launcher — that
  the options crossed the `LoadOptions` boundary, that the driver stayed
  resident after the launching app exited, that `status` sees it, and that
  `serve-stop` really closes the listener (the guest is kept alive past the
  stop, so a refused connection is evidence about the listener rather than
  about QEMU going away).
