/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* fs-read.h — shared identity + helper for the resident-driver file-read
 * fixture. Included by fs-read-driver.c, fs-read-probe.c, and fs-read-common.c
 * so the shared-driver GUID derivation agrees on both sides and both images
 * run the identical read chain. */

#ifndef FS_READ_H
#define FS_READ_H

#define FSREAD_NAME         "axl/fs-read"
#define FSREAD_DEFAULT_PATH "fs0:\\dof_in.txt"

/* Read the first line of @p path via axl_fopen -> axl_text_stream_wrap ->
 * axl_readline (the exact `-f<file>` consumer chain) and print, prefixed with
 * @p tag: FSREAD:<tag>-info / -open / -line. Defined in fs-read-common.c and
 * linked into BOTH the launcher (standalone context) and the resident driver,
 * so the two run byte-identical code. */
void
fsread_report(
    const char  *tag,   ///< output prefix ("app" launcher, "drv" driver)
    const char  *path   ///< file to read (UTF-8)
);

#endif /* FS_READ_H */
