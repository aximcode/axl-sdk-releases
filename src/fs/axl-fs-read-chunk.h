/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-fs-read-chunk.h
 * @brief Internal: the largest single file Read AXL issues.
 *
 * Every read AXL makes of a file on a volume -- the backend's
 * axl_backend_file_read, the driver loader's driver_file_read and the pack
 * stub's re-read of its own file -- asks the firmware for at most
 * AXL_FS_READ_CHUNK bytes per EFI_FILE_PROTOCOL.Read / Shell ReadFile call,
 * looping until it has what the caller asked for.
 *
 * Why: on a server booted through BMC virtual media, large single reads came
 * back intermittently corrupt while reads of 64 KB or less were reliable.
 * Measured on real hardware, fresh boots, no AXL network code running: a
 * packed tool loaded by the firmware's LoadImage (one read of the whole file)
 * from the virtual media failed its CRC 7 times in 10; the same file ran 10
 * of 10 times from a local USB copy and 8 of 8 from a RAM disk; and sha256sum
 * of it on the virtual media (64 KB reads) was correct 5 times in 5. An
 * UNPACKED image has no CRC, so the same fault runs the bad bytes instead.
 *
 * Header-only, no dependencies: the pack stub links none of libaxl.
 */

#ifndef AXL_FS_READ_CHUNK_H
#define AXL_FS_READ_CHUNK_H

/** Bytes per file Read call; see the file comment for the measurement. */
#define AXL_FS_READ_CHUNK  (64u * 1024u)

#endif /* AXL_FS_READ_CHUNK_H */
