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
