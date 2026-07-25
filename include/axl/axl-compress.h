/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-compress.h:
 *
 * AxlCompress — DEFLATE-family compression (RFC 1951) with gzip
 * (RFC 1952) and zlib (RFC 1950) framing. Backed by a vendored
 * single-header codec (sdefl/sinfl); AXL owns the framing and verifies
 * the CRC-32 / Adler-32 integrity fields via AxlDigest.
 *
 * This header is the one-shot whole-buffer layer — axl_compress /
 * axl_decompress. (A stream-filter layer over AxlStream — so tar.gz,
 * HTTP gzip, and file compression compose for free — is planned on top
 * of these.)
 *
 * Format coverage: GZIP, ZLIB, raw DEFLATE, and LZMA "alone" (.lzma /
 * GUIDED-LZMA). LZ4 may follow; zstd / xz are deliberately out of scope
 * (their encoders dwarf a UEFI tool).
 *
 * @code
 * void  *gz;
 * size_t gz_len;
 * if (axl_compress(AXL_COMPRESS_GZIP, data, len,
 *                  AXL_COMPRESS_LEVEL_DEFAULT, &gz, &gz_len) == AXL_OK) {
 *     // gz/gz_len is a complete .gz member; ships to disk or the wire.
 *     axl_free(gz);
 * }
 * @endcode
 */

#ifndef AXL_COMPRESS_H
#define AXL_COMPRESS_H

#include <stddef.h>
#include <axl/axl-macros.h>
#include <axl/axl-stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Container/framing format for a DEFLATE stream.
 */
typedef enum {
    AXL_COMPRESS_GZIP        = 0,  /**< gzip (RFC 1952): magic + CRC-32 + size */
    AXL_COMPRESS_ZLIB        = 1,  /**< zlib (RFC 1950): 2-byte header + Adler-32 */
    AXL_COMPRESS_DEFLATE_RAW = 2,  /**< bare DEFLATE (RFC 1951): no header/trailer */
    AXL_COMPRESS_LZMA        = 3   /**< LZMA "alone" (.lzma): 1-byte props + 4-byte
                                        dict size + 8-byte uncompressed size + data.
                                        Matches EDK2 GUIDED-LZMA (EE4E5898-…) and
                                        Python lzma FORMAT_ALONE. Encode + decode. */
} AxlCompressFormat;

/**
 * Use the codec's default effort. Explicit levels run 0 (store/fast)
 * through 9 (best); values are clamped into the codec's supported range.
 */
#define AXL_COMPRESS_LEVEL_DEFAULT  (-1)

// ---------------------------------------------------------------------------
// One-shot whole-buffer codec
// ---------------------------------------------------------------------------

/**
 * @brief Compress a whole buffer into a freshly allocated buffer.
 *
 * Produces a complete @p fmt stream: for GZIP a standalone `.gz`
 * member (10-byte header, DEFLATE body, CRC-32 + ISIZE trailer); for
 * ZLIB a 2-byte header + body + Adler-32; for DEFLATE_RAW the bare
 * compressed body with no framing.
 *
 * On success @p *out points to a heap buffer of @p *out_len bytes that
 * the caller frees with axl_free(). On failure @p *out is set to NULL
 * and @p *out_len to 0. An empty input (@p in_len == 0) is valid and
 * yields the framing for an empty payload.
 *
 * @return AXL_OK on success; AXL_ERR on allocation failure, NULL
 *     output pointers, or @p in_len exceeding the codec's limit.
 */
int
axl_compress(
    AxlCompressFormat fmt,      ///< output framing
    const void       *in,       ///< input bytes (may be NULL iff in_len == 0)
    size_t            in_len,   ///< input length
    int               level,    ///< 0..9, or AXL_COMPRESS_LEVEL_DEFAULT
    void            **out,      ///< [out] allocated compressed buffer (axl_free)
    size_t           *out_len   ///< [out] compressed length
);

/**
 * @brief Decompress a whole @p fmt stream into a freshly allocated buffer.
 *
 * The expected framing is selected by @p fmt — this does not
 * auto-detect (callers that need gzip-vs-plain sniffing should test the
 * `1f 8b` magic themselves and pick the format). gzip optional header
 * fields (FEXTRA / FNAME / FCOMMENT / FHCRC) are skipped, so members
 * written by the `gzip` command (which embed the original filename)
 * decode cleanly.
 *
 * Integrity: GZIP verifies the trailer CRC-32 and uncompressed size,
 * ZLIB verifies the Adler-32 — a mismatch is an AXL_ERR. DEFLATE_RAW
 * carries no checksum, so truncated raw input can silently yield a
 * short result; prefer a framed format when integrity matters.
 *
 * On success @p *out points to a heap buffer of @p *out_len bytes
 * (caller frees with axl_free); on failure @p *out is NULL and
 * @p *out_len is 0. A stream whose payload is empty yields @p *out_len
 * == 0 with a non-NULL one-byte allocation.
 *
 * @return AXL_OK on success; AXL_ERR on malformed framing, a checksum
 *     or size mismatch, corrupt DEFLATE data, or allocation failure.
 */
int
axl_decompress(
    AxlCompressFormat fmt,      ///< expected input framing
    const void       *in,       ///< compressed bytes
    size_t            in_len,   ///< compressed length
    void            **out,      ///< [out] allocated plaintext buffer (axl_free)
    size_t           *out_len   ///< [out] plaintext length
);

// ---------------------------------------------------------------------------
// Stream filters over AxlStream
// ---------------------------------------------------------------------------

/**
 * @brief Wrap @p sink as a compressing write stream.
 *
 * Bytes written to the returned stream are buffered; the framed @p fmt
 * stream is produced and written to @p sink when the writer is
 * finalized — either explicitly via axl_compress_writer_finish() (which
 * surfaces compression/write errors) or implicitly on axl_fclose().
 *
 * @p sink is borrowed, not owned: the caller closes it after the writer
 * is finalized/closed. Composes with anything — a file stream, an
 * in-memory buffer (axl_bufopen), a tar writer's backing stream.
 *
 * @return a write stream (free with axl_fclose), or NULL on bad
 *     arguments or allocation failure.
 */
AxlStream *
axl_compress_writer(
    AxlCompressFormat fmt,    ///< output framing
    AxlStream        *sink,   ///< destination for the compressed stream (borrowed)
    int               level   ///< 0..9, or AXL_COMPRESS_LEVEL_DEFAULT
);

/**
 * @brief Finalize a compressing writer: compress everything written so
 *        far and emit the framed stream to its sink.
 *
 * Idempotent — a second call is a no-op that returns the same status.
 * axl_fclose() calls this implicitly if it wasn't called explicitly,
 * but only an explicit call can report a compression or sink-write
 * failure to the caller.
 *
 * @return AXL_OK on success; AXL_ERR if @p s is not a compressing
 *     writer, or on compression / sink-write failure.
 */
int
axl_compress_writer_finish(
    AxlStream *s   ///< stream returned by axl_compress_writer
);

/**
 * @brief Wrap @p src as a decompressing read stream.
 *
 * Eagerly drains @p src, decompresses the whole @p fmt stream, and
 * returns a readable, seekable stream over the plaintext. Integrity is
 * verified during construction (a bad checksum/size yields NULL), so a
 * successful return means the data decoded cleanly.
 *
 * @p src is borrowed and read to EOF but not closed — the caller still
 * owns and closes it.
 *
 * @return a read stream over the plaintext (free with axl_fclose), or
 *     NULL on a decode error or allocation failure.
 */
AxlStream *
axl_compress_reader(
    AxlCompressFormat fmt,   ///< expected input framing
    AxlStream        *src    ///< compressed source (borrowed, read to EOF)
);

/** @brief Convenience: axl_compress_writer with AXL_COMPRESS_GZIP. */
AxlStream *
axl_gzip_writer(
    AxlStream *sink,   ///< destination for the gzip stream (borrowed)
    int        level   ///< 0..9, or AXL_COMPRESS_LEVEL_DEFAULT
);

/** @brief Convenience: axl_compress_reader with AXL_COMPRESS_GZIP. */
AxlStream *
axl_gzip_reader(
    AxlStream *src   ///< gzip source (borrowed, read to EOF)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_COMPRESS_H */
