/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-eof.h
    Internal: clear a stream's sticky end-of-file flag and nothing else.

    The one place a backend reaches past the public stream API. The file
    backend needs it after axl_ftruncate grows the file under a stream
    whose last read hit the old end: the flag then describes a length the
    file no longer has. The public ways to clear it do more -- axl_fseek
    discards transcode bytes still pending, axl_clearerr clears the error
    flag -- so neither can stand in for it.
**/

#ifndef AXL_STREAM_EOF_H
#define AXL_STREAM_EOF_H

#include <axl/axl-stream.h>

/** Clear @p s's end-of-file flag; NULL-safe. Touches nothing else. */
void
_axl_stream_clear_eof(
    AxlStream *s  ///< stream
);

#endif /* AXL_STREAM_EOF_H */
