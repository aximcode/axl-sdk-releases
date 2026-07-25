/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-file-gen.h
    Internal: the in-image file-generation registry.

    A cached reader (AxlFileView) that is already open when something
    else writes the file it reads has no way to notice: UEFI has no
    file-change notification, and re-stat'ing on every read is exactly
    the per-operation firmware round trip the cache exists to avoid.

    This registry is a cheap partial answer. Every AXL write path bumps a
    counter keyed on the file it wrote; a reader records the counter at
    open and compares ONE integer per read. A move costs a memory load and
    a compare, never firmware I/O; only a reader whose counter actually
    moved pays for a re-stat.

    ## It is an OPTIMISATION, not the guarantee

    AxlFileView promises CLOSE-TO-OPEN consistency -- a freshly opened
    view sees current contents, and re-opening is the only way to be sure
    of that. Coherence for an ALREADY-OPEN view is explicitly best-effort,
    and this registry is what makes the best effort good in the common
    case. Nothing may be built on it firing. See axl-file-view.h for the
    contract; do not strengthen it here.

    ## Keying

    The key is a hash of the path's FINAL COMPONENT, ASCII-case-folded.
    That is deliberately coarse. The asymmetry is what drives it:

      - A false POSITIVE (two unrelated files sharing a slot) costs one
        unnecessary re-stat. Harmless.
      - A false NEGATIVE (a write that a reader of the same file does not
        see) is the bug this exists to prevent.

    So the key must be invariant under every spelling of the same file.
    Hashing the whole path is not: "fs0:\\d\\f", "fs0:/d/f", "d\\f" and
    ".\\d\\f" all name one file and would hash four ways. The final
    component is the same under all of them, and case folding matches
    FAT's own rule for the names these paths are built from.

    Two unrelated files with the same basename in different directories
    therefore share a generation, as do basenames that collide in the
    fixed slot table. Both degrade to over-invalidation, which is SAFE for
    correctness but is not free, and the cost is worth stating plainly: a
    collision between a hot writer and a hot reader makes every read pay a
    firmware stat AND a stream close/reopen, which is strictly worse than
    the stat-per-read this mechanism exists to avoid. It is not
    hypothetical -- a WebDAV COPY that keeps the filename keys source and
    destination to the same slot, which is why axl-http-serve-fs.c's copy
    loop pins its source view. Choose basenames accordingly in any code
    that reads one file while writing another in a tight loop.

    Two residual FALSE NEGATIVES, neither a regression (the per-server
    marking this replaced compared with axl_strcasecmp and had both), but
    both real:

      - FAT 8.3 short names. "LONGFI~1.TXT" and "longfilename.txt" are one
        file and key two ways. Nothing in AXL generates short names.
      - Non-ASCII case. FAT LFN is case-insensitive over Unicode via the
        volume's upcase table; folding only A-Z means "Ä.txt" and "ä.txt"
        are one file keying two ways.

    ## Bounds

    A fixed AXL_FILE_GEN_SLOTS-entry array in BSS. No allocation ever —
    not at bump time, not at read time — so a write path can bump without
    a failure mode, and the registry adds no per-file state that could
    grow without limit.

    ## The ceiling

    The registry is PER-EFI-IMAGE, and the boundary is the PE image, not
    the process or the machine. mGen below is one static array per linked
    copy of libaxl: an application and a driver it loads are two images
    with two registries, and neither can see the other's. So a writer in
    any other image -- the UEFI Shell, another application, an embedded or
    separately loaded driver, the firmware itself -- bumps its own copy and
    is invisible here. A non-AXL writer bumps nothing at all.

    None of that is a gap to be closed. It is exactly why the documented
    guarantee is close-to-open: a reader that must not miss a foreign write
    re-opens, which works against every writer unconditionally. Making the
    registry cross-image would improve the best effort; it would not change
    what AxlFileView promises, and nothing should be written as though it
    did.

    Counters are uint32_t and wrap. A reader would have to sit through
    exactly 2^32 bumps of its slot between two reads to miss one.
**/

#ifndef AXL_FILE_GEN_H
#define AXL_FILE_GEN_H

#include <stddef.h>
#include <stdint.h>

/** Generation slots. Power of two; the key is masked into this range. */
#define AXL_FILE_GEN_SLOTS  256u

/**
 * @brief Compute the generation key for a path.
 *
 * Cheap enough to call per open; a write path that writes the same file
 * repeatedly should compute it once and use axl_file_gen_bump_key.
 *
 * @return the key (already usable with the _key calls; NULL path → 0).
 */
uint32_t
axl_file_gen_key(
    const char *path   ///< file path (UTF-8), any spelling
);

/**
 * @brief Read the current generation of @a key's slot.
 *
 * @return the counter. Compare it against a previously read value: equal
 *     means no in-image write path has touched a file keyed here since.
 */
uint32_t
axl_file_gen_read(
    uint32_t key   ///< key from axl_file_gen_key
);

/**
 * @brief Record that a file keyed by @a key was mutated.
 */
void
axl_file_gen_bump_key(
    uint32_t key   ///< key from axl_file_gen_key
);

/**
 * @brief Record that @a path was mutated. NULL-safe no-op.
 */
void
axl_file_gen_bump(
    const char *path   ///< file path (UTF-8)
);

#endif /* AXL_FILE_GEN_H */
