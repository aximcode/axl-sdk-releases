# AxlFs — filesystem operations

Path-based file and directory APIs, volume enumeration, file
metadata. Mirrors the POSIX split: the sibling **AxlStream**
module (`<axl/axl-stream.h>`) is the `<stdio.h>` analog (`FILE *`
/ streams); this module is the `<sys/stat.h>` + `<dirent.h>` +
`<sys/statvfs.h>` analog.

What's here:

- Whole-file convenience: `axl_file_get_contents`, `axl_file_get_bytes`,
  `axl_file_set_contents`, `axl_file_write_atomic` (temp-file + replace,
  never a half-written target and never a lost one).
- Incremental writes: `axl_file_writer_open` / `_write` / `_tell` /
  `_close` — out-of-core uploads that can't fit a whole-buffer write.
- File metadata: `AxlFsEntry`, `axl_file_info`, `axl_file_is_dir`,
  `axl_file_set_mtime`.
- File operations: `axl_file_delete`, `axl_file_rename` (rename or move
  within a volume, never replacing), `axl_file_replace` (`rename(2)`:
  replaces, and puts both entries back if it fails), `axl_file_move`
  (`mv`: a replace that copies across volumes), `axl_file_truncate`.
- The same calls for a file held open in an `AxlStream` from `axl_fopen`:
  `axl_fstat`, `axl_ftruncate`, `axl_fset_mtime` -- `fstat(2)`,
  `ftruncate(2)` and `futimens(2)`. They act on the file the stream was
  opened on wherever it now is, and `axl_fstat` reports its current name.
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

## Path length

A path is limited by the volume's own driver and by AXL, whichever is
lower; past the limit a call FAILS, it is never cut short:

- **FAT** (the file system UEFI requires): 260 characters for the whole
  path (EDK2's `EFI_PATH_STRING_LENGTH`).
- **AXL**: 511 characters relative to the volume (everything after
  `fsN:`; `AXL_BACKEND_VOL_PATH_MAX`). The no-Shell resolver, canonical
  names, rename and replace build the volume-relative path in buffers of
  that size and return `AXL_ERR` when it does not fit.
- **A volume published through AxlFsProvider** -- the 9P mount among them
  -- takes 511 UCS-2 units (`AXL_FS_PROVIDER_PATH_MAX` bytes of UTF-8),
  `EFI_INVALID_PARAMETER` past that, for the Shell's own commands too.

So the 511-character ceiling matters only on a non-FAT volume, in practice
a 9P mount of a deep host tree: a file deeper than that is not reachable
through AXL, and the Shell cannot create or enter a directory past it on
an AxlFsProvider volume. Lifting it means making path handling dynamic
through the backend's resolution, AxlFs and the provider thunk; it is
recorded as a known limit, not planned.

## Path resolution works with no shell at all

Which firmware interface resolves a path depends on what is present,
and the backend picks in this order:

| Context | Resolver | Volume naming |
|---------|----------|---------------|
| Modern UEFI Shell 2.x | the shell's map (`GetDevicePathFromMap`) + `OpenVolume`, resolved as `OpenFileByName` resolves | the shell's own alias, verbatim (`FS0:`) |
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

Either separator works in all three contexts: `fs0:/dir/file`,
`fs0:\dir\file` and any mixture name the same file. UEFI separates with `\`
only -- the modern shell's `OpenFileByName` fails `fs0:/x` outright, and the
old shell's map lookup splits on `\` alone -- so the backend turns `/` into
`\` on a copy of the path at each entry point that takes one (open, which
also serves delete, rmdir, stat and the directory test; mkdir; volume
location; `axl_chdir`), before choosing a resolver. No name can hold a `/`:
FAT refuses it, and `axl_dir_read` skips a listed name that has one.

## Under the modern Shell, a path operation costs the Shell nothing

The EDK2 Shell hands the results of several `EFI_SHELL_PROTOCOL` calls back
through a deferred-free list it empties only when the CURRENT SHELL COMMAND
ends: `GetCurDir`, `GetEnv`, `GetMapFromDevicePath` -- and through them
`GetDevicePathFromFilePath` on a relative path and `OpenFileByName` /
`CreateFile` on every path (the root they open is named through
`GetMapFromDevicePath`). Every one of those calls held a pool buffer until
the command ended. A short command got it all back; an app looping for
hours, or a resident driver (an SFTP server, a service) whose calls park
onto whatever command is current while the Shell idles at its prompt, grew
without bound. Measured per 400 operations inside one command, both arches:
25 pages for an absolute read, write, stat or directory listing, 50 for a
relative one, 75/175 for a mkdir and rmdir, 50/125 for a canonical-name
lookup, 25 for `axl_volume_enumerate` or `axl_get_current_dir`.

So AXL resolves paths under the modern Shell itself, from calls that park
nothing, reproducing the Shell's own algorithm string for string
(`EfiShellGetDevicePathFromFilePath`): the name before the first `:` is
looked up with `GetDevicePathFromMap` (the map's own device path), its
`SimpleFileSystem` volume is opened, and the rest of the path is opened
from the root with the caller's mode -- and, for a create, its attributes.
A path with no `:` is joined onto the current directory first. Every
answer is the Shell's, quirks included: map names in any case and any
alias, `fs0:dir` relative to the volume root (the Shell keeps no
per-volume current directory for this), a one-character remainder naming
the root (`fs0:\`, and `fs0:a` too), a leading `\` climbing to the current
volume's root, `NUL`/`>e`/`>v...` still answered by the Shell itself. The
handle is the volume's own `EFI_FILE_PROTOCOL`, which is what the Shell's
handle for a file is, so reads, writes and closes are unchanged.

Two answers the Shell can only give by parking are found another way:

- The current directory is cached, and the cache checks itself on every
  use without parking. The Shell mirrors the directory into a volatile UEFI
  variable (`cwd` under EDK2's `gShellVariableGuid`), written by every
  change -- this image's `axl_chdir`, a child image's, the user's `cd` at
  the prompt while a resident driver runs. A cached answer is reused while
  the variable still holds exactly it and its volume is still mapped; an
  answer the variable does not vouch for is not kept. A Shell that keeps no
  such variable is asked every time -- what AXL did before.
- A volume's `fs<n>` name (`axl_volume_map_name`, and so
  `axl_volume_enumerate`) is the first `fs<n>` on the Shell's list of the
  volume's names, and that list cannot be cached: setting a name the volume
  already has moves it to the end, which no non-parking call shows. So
  `FS0:`..`FS63:` are each looked up (`GetDevicePathFromMap`); when exactly
  one maps to the volume, order cannot matter and it is the answer.
  Otherwise -- no `fs<n>` name, or two -- the Shell is asked, as before.

Still asked of the Shell, so still one parked buffer per call:
`axl_volume_map_alias` (the FIRST name, which any re-set reorders),
`axl_volume_map_name` for a volume with no or several `fs<n>` names,
`axl_get_current_dir` and relative paths while the current directory's
volume is unmapped (or on a Shell with no `cwd` variable),
`axl_getenv`, `axl_volume_unmap_device`'s name walk and
`axl_shell_execute`, and `axl_get_current_dir` while the current directory
is 512 characters or longer -- which only a non-FAT volume can reach, and
from which relative operations fail anyway (see "Path length" below). None
is a per-file-operation path in the normal case.

What the checks cannot see, and so where an answer could differ from the
Shell's: an `fs<n>` name outside `FS0:`..`FS63:` (fs64 and up, or a spelling
like `fs07`) on a volume that also has one inside; a name set for a device
path equal to the volume's only as text; and a `cwd` variable put back by
something other than a `SetCurDir` -- EDK2's `Execute` called with an
`Environment` restores the variables but not its own list (AXL passes
none). `test-shell-buffer-growth-qemu.sh` measures every class above at 0
pages per 400 operations, against a raw `OpenFileByName` control that still
parks, and pins every tricky case's outcome to what the Shell itself
answered.

## Reads are made 64 KB at a time

Every AXL read of a file -- `axl_file_get_contents`, `AxlStream` file
reads, directory reads, all through the backend's `axl_backend_file_read`
-- asks the firmware for at most 64 KB per `ReadFile` / `Read` call
(`AXL_FS_READ_CHUNK`, in the internal `src/fs/axl-fs-read-chunk.h`) and
loops until it has what the caller asked for. Callers see no difference:
the contract is still "fill the buffer, or stop at end of file".

Why: on a server booted through BMC virtual media, large single reads come
back intermittently corrupt while reads of 64 KB or less are reliable.
Measured on real hardware, fresh boots, no AXL network code running: a
packed tool loaded by the firmware's `LoadImage` (one read of the whole
file) failed its CRC 7 times in 10, ran 10 of 10 times from a local USB copy
and 8 of 8 from a RAM disk, and `sha256sum` of it on the virtual media (64
KB reads) was correct 5 times in 5. The same limit applies to the driver
loaders, which read an image themselves rather than let the firmware read
it (`src/util/README.md`), and to a packed image's stub when it re-reads
its own file (`src/pack/README.md`).

The loop stops at a SHORT read -- fewer bytes than that call asked for --
as well as at the requested size. For a file a short read is end of file;
a directory returns one entry per call and a pipe what it holds, so a
request of up to 64 KB is still exactly one call, as before.

A read that fails part-way keeps what it read, as POSIX `read()` does: the
bytes before the failing chunk are returned as a short read (the file
position has already passed them), and the NEXT call retries the device from
the failing chunk, reporting the error if the fault recurs. So
an `AxlStream` read delivers them and sets `axl_ferror()`, while
`axl_file_get_contents` -- which needs the whole file -- returns
`AXL_IO_ERROR` whenever it got fewer bytes than the file's size, never the
bytes before the error as if they were the file.

Tested end to end by `test-vmedia-reads-qemu.sh`, whose fixture volume
flips a byte in any single read over 64 KB and, for `ioerr-*` files, fails
every read past the first chunk.

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
- `axl_file_replace`'s copy (and so `axl_file_move`'s) flushes the
  destination BEFORE deleting the source, and a copy that fails is removed
  and the entry it was replacing put back, so a failed flush costs a retry
  rather than either file.
- `axl_file_writer_close` flushes explicitly and folds the status into
  its return (which is why a WebDAV PUT over it can answer 500 rather
  than 201 for data that never landed).
- `axl_file_truncate` flushes too: re-reading the length through the same
  open handle proves the driver accepted it, not that it reached the
  media. So do `axl_ftruncate`, `axl_file_set_mtime` and `axl_fset_mtime`.

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

A write that starts past the end of a file -- `axl_pwrite` at a larger
offset, or `axl_fseek` past the end then `axl_write` -- leaves zeros in
the gap, as a POSIX hole reads. The FAT driver alone would not: it
extends a file on write without writing the gap, which exposed whatever
the newly allocated clusters held before, so the file stream writes the
zeros itself.

## Rename, replace and move

`axl_file_rename(old, new)` is `rename(2)` with `RENAME_NOREPLACE`. `new`
is either a bare name, which renames in place, or a path anywhere on the
same volume -- including another directory -- and the entry moves in one
`SetInfo` whatever its size; a directory moves with everything below it,
and times and attributes are kept. It never replaces an existing entry:
a taken name is `AXL_EXISTS`.

"Same volume" is decided after both paths are resolved -- the current
directory, either separator, any map name or alias for the volume -- not
by comparing the strings. A path on another volume is `AXL_UNSUPPORTED`
with nothing changed, the `EXDEV` case; so is a move between directories
on an `AxlFsProvider` volume whose provider has no `rename` callback (a
rename within one directory works there).

`AXL_OK` is verified -- the entry is re-read through the same handle and
must carry the new name -- so a FAT rename that changes only letter case,
which the driver treats as the same name and ignores, is `AXL_ERR`. FAT
also refuses to move the volume root, a directory into itself or below
it, a directory with any file below it open, and a read-only source. A
path whose `..` climbs above the volume root is `AXL_ERR` too.

One failure "nothing changed" cannot cover: the EDK2-derived FAT driver
removes the old directory entry before it writes the new one, so a
destination directory that must grow on a full volume can fail in between
and leave the entry under neither name. AXL cannot reorder the driver's
writes; it logs an error when it finds the entry gone from both.

`axl_file_replace(old, new, flags, &result)` is `rename(2)` itself: an
existing `new` is REPLACED. FAT cannot rename onto a taken name, so the
replace is a sequence -- every refusal decided first; the entry at `new`
renamed ASIDE to `~AXLhhhh.TMP` in its own directory (a pure 8.3 name, so
never another entry's generated alias); a check that `old` still resolves
and `new` no longer does; the source renamed (or copied) into place; and
only then the aside removed. A failure before that last step puts the
aside back, so no failure AXL can observe costs either entry. A Ctrl-C in
the middle does not end the program there: the sequence is undone (or, past
the point of no return, finished) and the call answers `AXL_CANCELLED`
with the interrupt still pending.

Whether the two paths name ONE entry is decided by the names the
filesystem stores, read through a handle on every component of both --
never by comparing strings -- so an 8.3 alias, `.\`, the other separator
or a trailing dot FAT drops is `AXL_OK` with `AXL_FILE_REPLACE_RESULT_SAME`
and nothing changed. A change of letter case in the last component only
is performed (`RECASED`). The kind rules are rename(2)'s, with rename(2)'s
codes: `AXL_NOT_DIR` for a path through a file or a directory onto a file,
`AXL_IS_DIR` for a file onto a directory, `AXL_NOT_EMPTY` for a directory
onto a non-empty one (or onto one holding the source), `AXL_INVALID` for a
directory into itself. `AXL_FILE_REPLACE_TREE` lifts the kind and
emptiness rules and replaces a whole tree -- WebDAV MOVE with
`Overwrite: T`. `AXL_FILE_REPLACE_COPY` copies a file (and, with `TREE`, a
directory tree) when no rename can move it: another volume, or a provider
without a `rename` callback asked to move between directories. A read-only entry or write-protected
volume is `AXL_DENIED`, and a volume reporting less free space than the
sequence may need -- the directory entries it appends and, for a copy,
the source rounded to whole clusters (`EFI_FILE_SYSTEM_INFO.BlockSize` is
the FAT cluster) -- is `AXL_NO_RESOURCES`, both before anything changes.
The header lists what the sequence still cannot prevent (a power loss
between the aside and its removal, a FAT32 free-space hint that
overstates, other writers).

`axl_file_move(old, new)` is `axl_file_replace(old, new,
AXL_FILE_REPLACE_COPY, NULL)`: `mv`. It takes the same steps, the
set-aside included: when `new` exists, that entry is first renamed to
`~AXLhhhh.TMP` in its own directory, the source is renamed -- or, where no
rename can move it, copied, flushed, given the source's time and the source
deleted -- into `new`, and only then is the aside removed; a rename or copy
that fails puts the aside back under its old name. It has rename(2)'s kind
rules too, with their codes: a directory onto a file is `AXL_NOT_DIR`, a
file onto a directory `AXL_IS_DIR`, a directory onto a non-empty one
`AXL_NOT_EMPTY`, a directory into itself `AXL_INVALID`; a directory moves
only by rename, never by copy. `axl_file_write_atomic` writes
"<path>.tmp", flushes it, and puts it in place with `axl_file_replace(...,
0)`, so a failed promote leaves the target exactly as it was.

`axl_file_delete` splits its failure in two for callers that map a status
onto something else (a WebDAV 404 against a 500): `AXL_NOT_FOUND` means
the volume was reached and holds no such entry, and nothing else;
`AXL_ERR` is every other failure -- a file that exists and cannot be
removed (the read-only attribute, a read-only volume), a volume that does
not resolve (an unmapped `fs9:`), NULL. It never CREATES the file on the way
to answering, which EDK2's `EfiShellDeleteFileByName` used to do.

## Modification times

`axl_file_set_mtime(path, t)` / `axl_fset_mtime(stream, t)` take Unix
seconds, UTC -- the value `AxlFsEntry.mtime_unix` reports, so a time read
from one file applies to another unchanged. FAT's range bounds them on
every volume -- `AXL_FS_MTIME_MIN` (1980-01-01) to `AXL_FS_MTIME_MAX`
(2107-12-31), anything outside `AXL_INVALID` before the volume is touched
-- and the volume sets the precision: FAT keeps two-second steps, an odd
second stored as the even one below; a provider may keep the second.
`AXL_OK` is verified and flushed; a time the filesystem did not keep -- a
provider whose `set_info` ignores `mtime_unix`, or FAT given a 1980 time
for a file stamped in another year (the driver keeps the old year) -- is
`AXL_ERR`, with the previous time written back. A read-only file is
`AXL_DENIED`. `AXL_FS_MTIME_KEPT_MIN` (1981-01-01) is the first second
every volume keeps, which is where a server that must clamp a time it was
sent -- SFTP's SETSTAT, 9P's Tsetattr -- clamps it.

File times are read and written as UTC calendar fields with the zone
unspecified. A time a driver reports WITH a zone is converted as UEFI 2.11
section 8.3 defines it, Localtime = UTC - TimeZone (so UTC = local +
TimeZone).

A later write through any handle stamps the current time again when that
handle is flushed or closed, as on POSIX, so set the time LAST:
`axl_fset_mtime` writes the stream's own buffered bytes out first, which
makes write, `axl_fset_mtime`, `axl_fclose` keep it.

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
  `axl_file_truncate`, `axl_ftruncate`, `axl_file_delete`, `axl_file_rename`,
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

Every name a provider lists is checked at the seam: one that is empty,
`.`, `..`, or holds `\` or `/` is not handed up -- joined onto a path,
`..\x` is three components and climbs out of the directory being walked,
and a 9P server passes Linux names like that through. Provider listings
therefore no longer include `.` and `..`. The others are reported with the
provider and the escaped name -- the first in a directory at warning, the
rest at debug. A `:` is allowed: inside a joined path it is a character,
not a volume. `axl_dir_read` applies the same check for every driver, and
also skips a name longer than 765 bytes of UTF-8 -- too long for
`AxlFsEntry.name` (`AXL_FS_NAME_MAX`, 766 with the NUL), and cut short it
could be a sibling's name -- with the same once-per-directory warning.
Every name FAT creates fits: 255 UTF-16 units are at most 765 bytes. A
longer one comes only from a non-FAT driver, a provider, or a FAT volume
written by a non-conforming tool; an entry too long even for the listing's
buffer is read into a heap buffer only to be stepped past, so the entries
after it are still listed. A provider must skip such a name itself: it
cannot hand one up in an `AxlFsEntry`, and a cut one is indistinguishable
from a real name.

### Version 2: renames, times, open mode

`AXL_FS_PROVIDER_VERSION` 2 adds an optional `rename(file, new_path)`
callback after `backend_ctx`. A provider with it gets EVERY name change a
SetInfo asks for -- in its directory or another -- as an absolute provider
path, so `axl_file_rename`, `axl_file_replace` and `axl_file_move` move an
entry between directories of the volume in one step instead of copying. A
provider without it keeps version 1's behaviour: a rename in place reaches
`set_info` as the new basename, and a move elsewhere is `EFI_UNSUPPORTED`
(`axl_file_move` copies). `axl_fs_provider_publish` copies only the
`struct_size` bytes a provider declares, so a version-1 vtable publishes
unchanged and is never read past its end.

The thunk resolves a SetInfo FileName as the EDK2-derived FAT driver does
-- a leading separator from the volume root, anything else from the file's
PARENT directory (the Shell's `mv` sends `..\x`) -- and refuses, without
calling the provider: the root, a path above it, a directory into itself,
a directory with any handle open below it (FAT refuses that too), a change
to the DIRECTORY bit, and, through a handle opened read-only, anything but
an attribute change (UEFI 2.11 13.5.14).

Times travel both ways: `get_info`'s `mtime_unix` reaches EFI callers as
`ModificationTime` (UTC fields, zone unspecified), and `set_info` is handed
a time only when one is asked for -- not for the all-zero `EFI_TIME` UEFI
uses for "leave it", nor for the current time echoed back by a rename or
an attribute change. A resize that echoes the old time passes it, so the
provider can restore it after the size change. `set_info` is called only
when something differs from what `get_info` reports.

Paths handed to a provider are at most `AXL_FS_PROVIDER_PATH_MAX` (1536)
bytes, each component no longer than `AxlFsEntry.name` holds; anything
longer is `EFI_INVALID_PARAMETER`, never cut.

Headers: `<axl/axl-fs-provider.h>`, `<axl/axl-device-path.h>`
(vendor device-path constructor used internally by `_publish`).
Worked example: `sdk/examples/memfs.c` (read-only RAM-disk in
~190 LOC).
