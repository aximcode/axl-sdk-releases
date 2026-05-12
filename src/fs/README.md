# AxlFs — filesystem operations

Path-based file and directory APIs, volume enumeration, file
metadata. Mirrors the POSIX split: the sibling **AxlStream**
module (`<axl/axl-stream.h>`) is the `<stdio.h>` analog (`FILE *`
/ streams); this module is the `<sys/stat.h>` + `<dirent.h>` +
`<sys/statvfs.h>` analog.

What's here:

- Whole-file convenience: `axl_file_get_contents`, `axl_file_set_contents`.
- File metadata: `AxlFileInfo`, `axl_file_info`, `axl_file_is_dir`.
- File operations: `axl_file_delete`, `axl_file_rename`.
- Directory ops: `axl_dir_mkdir`, `axl_dir_rmdir`,
  `axl_dir_open` / `axl_dir_read` / `axl_dir_close`,
  `axl_dir_walk`, `axl_dir_list_json`.
- Volume enumeration: `AxlVolume`, `axl_volume_enumerate`,
  `axl_volume_get_label`, `axl_volume_get_label_by_handle`.

All paths are UTF-8; backend converts to UCS-2 internally.
High-level wrappers (`axl_file_get_contents`) layer on top of
`axl_fopen` — they're path-based shortcuts, not stream
primitives.

Header: `<axl/axl-fs.h>`

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
filesystem they can `dir` / `cd` / `LoadImage` against.

Headers: `<axl/axl-fs-provider.h>`, `<axl/axl-device-path.h>`
(vendor device-path constructor used internally by `_publish`).
Worked example: `sdk/examples/memfs.c` (read-only RAM-disk in
~190 LOC).
