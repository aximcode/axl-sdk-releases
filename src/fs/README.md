# AxlFs — filesystem operations

Path-based file and directory APIs, volume enumeration, file
metadata. Mirrors the POSIX split: the sibling **AxlStream**
module (`<axl/axl-stream.h>`) is the `<stdio.h>` analog (`FILE *`
/ streams); this module is the `<sys/stat.h>` + `<dirent.h>` +
`<sys/statvfs.h>` analog.

What's here:

- Whole-file convenience: `axl_file_get_contents`, `axl_file_get_bytes`,
  `axl_file_set_contents`, `axl_file_write_atomic` (temp-file + rename,
  never a half-written target).
- Incremental writes: `axl_file_writer_open` / `_write` / `_tell` /
  `_close` — out-of-core uploads that can't fit a whole-buffer write.
- File metadata: `AxlFsEntry`, `axl_file_info`, `axl_file_is_dir`.
- File operations: `axl_file_delete`, `axl_file_rename`,
  `axl_file_move`, `axl_file_truncate`.
- Directory ops: `axl_dir_mkdir`, `axl_dir_rmdir`,
  `axl_dir_open` / `axl_dir_read` / `axl_dir_close`,
  `axl_dir_walk`, `axl_dir_list_json`.
- Volume enumeration: `AxlVolume`, `axl_volume_enumerate`,
  `axl_volume_get_label`, `axl_volume_get_label_by_handle`.
- Volume space: `axl_volume_get_space`, `axl_volume_get_space_by_handle`.
- Shell mappings: `axl_volume_map_name` / `_map_alias` (what is this
  volume called?), `axl_volume_map_taken`, `axl_volume_set_map` /
  `_alias_to_fsn` / `_unmap` (name a volume, or drop the name) — the
  programmatic equivalent of the shell's `map` command, usable without
  a `map -r` cycle.

All paths are UTF-8; backend converts to UCS-2 internally.

## Path resolution works with no shell at all

Which firmware interface resolves a path depends on what is present,
and the backend picks in this order:

| Context | Resolver | Volume naming |
|---------|----------|---------------|
| Modern UEFI Shell 2.x | `EFI_SHELL_PROTOCOL.OpenFileByName` | the shell's own alias, verbatim (`FS0:`) |
| Old EFI 1.x shell | `SHELL_ENVIRONMENT.GetMap` + `EFI_FILE_PROTOCOL` | the old shell's map (`fs0`) |
| No shell — a BDS boot option | `LocateHandleBuffer(SimpleFileSystem)` + `OpenVolume` | positional (`fs0`, `fs1`, ...) |

The third row is what makes an app launched straight from
`\EFI\BOOT\BOOTx64.EFI` work: it has no shell, so `axl_app_image_path`
names its volume positionally and every path op resolves through the raw
filesystem handles. That positional namespace is the same one
`axl_volume_enumerate` already falls back to when no mapping covers a
handle, so the two agree.

It is **not** guaranteed to agree with what a shell would call the same
volume — the `LocateHandleBuffer` order and the shell's `fsN` order do
diverge in practice (measured: with two volumes, the handle at index 1
was the one the shell called `FS0:`). So the positional naming is gated
on there being no shell at all. Where a shell IS live its map is the only
naming used, and a volume it has not mapped is reported as unnamed rather
than given a positional name the shell would resolve to a *different*
volume.

Names are resolved per call rather than cached, because the positional
namespace is only stable while the volume set is: a volume that goes away,
or a device disconnect/reconnect cycle, renumbers the ones after it (a
`map -r` can likewise re-letter a shell's). Within one boot and one volume
set the namespace is self-consistent, which is all a path needs to be.

Without a shell there is no current working directory, so a path must
name its volume. `fs0:\dir\file` resolves; a bare `\dir\file` is refused
rather than silently taken to mean volume 0.

## Durability: a write is not done until it is flushed

Closing a file tells you nothing about whether its bytes reached the
media — `EFI_FILE_PROTOCOL.Close` is specified to return only
`EFI_SUCCESS`, so the backend's close cannot report a full volume,
write-protected media or a device error even when it hit one. The
flush is where that status lives.

Every write path in this module therefore flushes and CHECKS it before
reporting success, and the paths that destroy something flush before
they destroy it:

- `axl_file_set_contents` flushes before it closes, so `AXL_OK` means
  the bytes are on the volume.
- `axl_file_write_atomic` promotes the temp over the target only after
  that succeeded — otherwise a failed write would replace a good file
  with a short one.
- `axl_file_move`'s copy fallback flushes the destination BEFORE
  deleting the source, so a failed flush costs a retry rather than the
  file.
- `axl_file_writer_close` flushes explicitly and folds the status into
  its return (which is why a WebDAV PUT over it can answer 500 rather
  than 201 for data that never landed).
- `axl_file_truncate` flushes too: re-reading the length through the same
  open handle proves the driver accepted it, not that it reached the
  media.

Consumers writing their own paths over `AxlStream` should do the same:
`axl_fflush` and check it, then `axl_fclose`. `axl_fclose` drains the
AXL-side buffer only — it never calls the stream's flush — so it is not
a durability point. See `src/stream/README.md`.

High-level wrappers (`axl_file_get_contents`) layer on top of
`axl_fopen` — they're path-based shortcuts, not stream
primitives.

`axl_file_truncate(path, size)` is the `truncate(2)` analog — it sets an
existing file's length either way, shrinking (the surviving prefix is
byte-exact) or growing. Two things it does *not* share with POSIX.
Growing is not a cheap metadata update: the FAT driver physically writes
the added region out as zeros, so growing by gigabytes writes gigabytes
and blocks for the duration — bound the size when it comes from
untrusted input. And the file's timestamps are preserved rather than
bumped. `AXL_OK` means the new length was re-read and verified on the
same open handle, so a filesystem that accepts the request and quietly
ignores it — an `AxlFsProvider` whose `set_info` only implements renames
and attribute changes — yields `AXL_ERR` instead of a hollow success.

Header: `<axl/axl-fs.h>`

## Volume queries — label and free space

Both `axl_volume_get_label` and `axl_volume_get_space` are properties of
the **volume**, not of the path you name it with, so every spelling of
one volume answers the same and the path need not exist: `"fs0:"`,
`"fs0:\"` and `"fs0:\dir\notyet.bin"` all resolve to the same volume
root. A path with no volume prefix resolves against the working
directory — but only a path that names *no* volume does, and only where
a shell supplies one (with no shell there is no working directory and
such a path is refused). One naming a volume too long to resolve fails
outright rather than falling back to the working directory, which would
answer about a different volume under a clean `AXL_OK`. Each has a
`_by_handle` sibling for the handles `axl_volume_enumerate` hands back,
since `AxlVolume.name` can fall back to a positional index that no shell
knows by that name.

```c
uint64_t total = 0, avail = 0;
switch (axl_volume_get_space(image_path, &total, &avail)) {
case AXL_OK:
    if (avail < needed) { /* refuse up front, with a number to report */ }
    break;
case AXL_UNSUPPORTED:
    /* This volume cannot say. Decide some other way -- do NOT read
       `avail`, it was never written. */
    break;
default:
    /* Bad path / unopenable volume / device error. */
    break;
}
```

The three-way return exists because a caller sizing a write has to tell
"1 KB left, refuse" from "this volume cannot say" — and both of those
from "that volume is broken". A volume answering `EFI_DEVICE_ERROR`,
`EFI_NO_MEDIA` or `EFI_VOLUME_CORRUPTED` is reported as `AXL_ERR`, never
laundered into `AXL_UNSUPPORTED`; only a volume with no filesystem
information to give, or one reporting a figure as unknown, is
`AXL_UNSUPPORTED`. A figure is written
**only** when the volume reported a real one — nothing is zero-filled
and no sentinel is handed back for the caller to recognise, so on any
non-`AXL_OK` return both out-parameters are untouched. Only the figures
you ask for gate the result: pass `NULL` for `total` and a volume that
knows its free space but not its size still answers. Provider-backed
volumes route through `AxlFsProviderVolumeInfo`, whose `(uint64_t)-1`
"unknown" markers surface as `AXL_UNSUPPORTED`.

## AxlFileView — mmap-like windowed view over a file

`<axl/axl-file-view.h>` opens a file read-only and serves reads from a
small LRU [page cache](../data/README.md) without ever loading the file
whole — the out-of-core property real `mmap` would give, but as a
software cache. (True MMU demand paging is unworkable in UEFI: a `#PF`
handler would have to do blocking filesystem I/O outside the TPL model,
and a FAT file has no physical backing to zero-copy map, so even a "real"
`mmap` would copy each page in through the FS driver anyway.)

Two access modes:

- `axl_file_view_read(v, offset, out, len)` — copy a range out,
  transparently spanning pages; clamped at EOF.
- `axl_file_view_page(v, offset, &avail)` — borrow a zero-copy pointer
  into the resident frame for the page containing `offset`, with `avail`
  bytes valid before the page boundary. Walk boundaries by re-calling at
  `offset + avail`. The pointer is valid until the next view call.

```c
AXL_AUTOPTR(AxlFileView) v = axl_file_view_open("fs0:\\big.log", 0, 16);
size_t total = axl_file_view_size(v);

char line[256];
size_t got = axl_file_view_read(v, total - 256, line, sizeof(line));
```

Read-only; for windowing large files (logs, the original text of an
out-of-core editor buffer). Header: `<axl/axl-file-view.h>`

### Consistency model: close-to-open

`AxlFileView` gives **close-to-open consistency** — the same guarantee NFS
gives by default:

- **Guaranteed: a freshly opened view sees the file's current contents.**
  Unconditionally — across EFI images, against a non-AXL writer, against
  the UEFI Shell, against the firmware. The open stats the file itself and
  a view's cached pages are keyed on its own identity and dropped when it
  closes, so nothing can defeat this. **To see current data, re-open the
  view.**
- **Not guaranteed: that a view already open when a write happens will
  notice it.** As a best effort it usually does, when the write went
  through AXL in the same PE image. Every AXL write path records that it
  touched a file — `axl_file_set_contents`, `axl_file_write_atomic`,
  `axl_file_truncate`, `axl_file_delete`, `axl_file_rename`,
  `axl_dir_mkdir`/`_rmdir`, `AxlFileWriter`, every `axl_fwrite` /
  `axl_pwrite` on a file stream, the file log handler — and a view
  compares one integer per access, dropping its pages and re-stat'ing only
  when the file it reads actually moved. A read that follows no write
  costs a compare, not a firmware round trip, which is why it is worth
  having. It is an optimisation, not a promise.

Do not build on the best-effort half. It does not fire for a writer in
another PE image (`libaxl` is statically linked, so an application and a
driver it loads have separate bookkeeping), nor for a non-AXL writer, nor
for the Shell — and no amount of reading will reveal such a write. It is
also only evaluated when the view is *asked* for data; a view nobody reads
notices nothing.

`axl_file_view_refresh(v)` runs that same best-effort check on demand, so
it does **not** reveal a write the view could not otherwise see — only
re-opening does. Its value is the return: `AXL_ERR` if the file was
deleted or renamed away, which is the only way to tell that apart from an
ordinary empty read, since a vanished file reports size 0.

`axl_file_view_set_pinned(v, true)` turns the best-effort half **off** for
one view, for a consumer that has already committed to a length or to byte
offsets (an HTTP body under an already-sent `Content-Length`, an editor
buffer indexing the original text) and would be corrupted rather than
refreshed by a concurrent write.

It cannot add any guarantee about the file's bytes — under close-to-open
there was never one to strengthen — so it is **not** a snapshot. A pin
bounds what the view *reports*, not what it can *deliver*:

- **Guaranteed: the length.** `axl_file_view_size` keeps answering with
  what the view last observed, so a `Content-Length` already on the wire
  stays honest and offset arithmetic derived from that length stays
  self-consistent.
- **Not guaranteed: the bytes.** A page not yet resident is read from the
  file as it stands when first touched. And a page that *is* resident can
  be evicted at any moment — [`AxlPageCache`](../data/README.md) picks its
  LRU victim across all frames with no regard for owner or pin, because
  evicting is what it is for. Reading past `max_frames` pages evicts the
  view's own earlier pages, and in a shared cache another tenant can evict
  them sooner; whatever re-faults is filled from the file as it stands
  then.

So a pinned view **can** hand back post-write bytes for a region it
revisits. UEFI offers no file-snapshot primitive to close that gap short
of copying the file whole, which is what a windowed view exists to avoid —
a consumer that cannot tolerate it must copy.

`axl_file_view_open_cached(path, cache)` opens a view that borrows a
caller-owned shared [`AxlPageCache`](../data/README.md) instead of
allocating its own frame pool, so many open files share one bounded set
of resident pages (the view adopts the cache's power-of-two page size and,
on close, returns only its own frames — the cache outlives it).

## AxlFsProvider — publish a UEFI-visible filesystem

The flip side of `AxlFs`: instead of *consuming* a filesystem the
firmware already published, `<axl/axl-fs-provider.h>` lets a
consumer *publish* one. Useful for HTTP-backed mounts (axl-webfs),
in-memory RAM-disks, archive viewers, mock filesystems for tests
— anything that wants Shell, LoadImage, and the Boot Manager to
see a `fsN:` mapping that's actually backed by application code.

Consumers fill an `AxlFsProvider` vtable in pure UTF-8 + snake_case
+ `AxlFsStatus` terms. `axl_fs_provider_publish` synthesizes the
matching `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` + `EFI_FILE_PROTOCOL`
vtables, marshals UCS-2 ↔ UTF-8 at the boundary, lays out
`EFI_FILE_INFO` / `EFI_FILE_SYSTEM_INFO` trailers in
caller-supplied buffers (with the spec's probe-then-resize
`EFI_BUFFER_TOO_SMALL` semantics), maps `AxlFsStatus` → spec
`EFI_STATUS` codes, and installs both protocols on a fresh handle.

The consumer never spells an `EFI_*` identifier. UEFI consumers
(Shell, Boot Manager, LoadImage) see a perfectly-conformant
filesystem they can `dir` / `cd` / `LoadImage` against. `_publish`
assigns the volume a real `fsN:` shell mapping immediately (via
`SetMap`, not a `map -r` refresh — a refresh driven programmatically
via `EFI_SHELL_PROTOCOL.Execute` only updates a nested child shell's
own throwaway map table, never the publishing image's persistent
one), so the caller can read/write through the mapping the moment
`_publish` returns, no extra step needed. `_unpublish` removes the
mapping again before freeing the underlying device path.

Headers: `<axl/axl-fs-provider.h>`, `<axl/axl-device-path.h>`
(vendor device-path constructor used internally by `_publish`).
Worked example: `sdk/examples/memfs.c` (read-only RAM-disk in
~190 LOC).
