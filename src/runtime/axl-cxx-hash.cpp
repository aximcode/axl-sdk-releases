/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-hash.cpp
    `std::_Hash_bytes` and `std::_Fnv_hash_bytes` -- the byte-range hash
    behind `std::hash` for strings and other non-scalar keys.

    Together with `axl-cxx-rbtree.cpp` these were the last two things a
    hosted C++ image took from `libstdc++.a` (`hash_bytes.o` and
    `tree.o`).  Supplying both lets a C++ image drop the archive
    entirely.  See `AXL-Cxx-Design.md` section 8 for why that matters:
    redistributing the runtime library is the one act the GCC Runtime
    Library Exception does not cover.

    CLEAN-ROOM under Apache-2.0.  `_Fnv_hash_bytes` is FNV-1a, named by
    the symbol itself and specified publicly (Fowler/Noll/Vo).
    `_Hash_bytes` is AXL's own choice of function, and it does not have
    to match libstdc++'s: nothing persists or exchanges these values.

    @par Why matching libstdc++ byte-for-byte is NOT required

    `std::hash` guarantees only that equal keys hash equally WITHIN one
    execution.  The standard explicitly permits the value to vary between
    program runs, and no on-disk or on-wire format in this SDK stores a
    `std::hash` result.  A firmware image links exactly one definition of
    these symbols, so every container inside it agrees with itself, which
    is the whole requirement.

    @par Why FNV-1a rather than something faster

    It is one multiply and one xor per byte with no table, no unaligned
    reads and no endianness dependence -- so it behaves identically on
    x64 and AArch64, which a word-at-a-time hash would not without care.
    The keys these actually see are short (paths, PCI ids, config keys),
    where a bulk hash's setup cost is not repaid.
**/

#include <stddef.h>
#include <stdint.h>

/* Declarations only, so a signature that drifts from libstdc++'s becomes a
   compile error here rather than a link error in a consumer. Header-only;
   pulls no libstdc++ objects. */
#include <bits/functional_hash.h>

namespace {

/* The 64- and 32-bit FNV parameters, as published. size_t picks the width. */
#if SIZE_MAX > 0xFFFFFFFFu
constexpr size_t FNV_OFFSET = static_cast<size_t>(14695981039346656037ULL);
constexpr size_t FNV_PRIME  = static_cast<size_t>(1099511628211ULL);
#else
constexpr size_t FNV_OFFSET = static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME  = static_cast<size_t>(16777619U);
#endif

inline size_t
fnv1a(const void *ptr, size_t len, size_t seed)
{
    const unsigned char *p = static_cast<const unsigned char *>(ptr);
    size_t               h = seed;

    for (size_t i = 0; i < len; i++) {
        h ^= static_cast<size_t>(p[i]);
        h *= FNV_PRIME;
    }

    /* Mix the LENGTH in. Plain FNV has an absorbing state at 0: if the
       running hash ever reaches zero, every subsequent NUL byte leaves it
       there, so "\0\0x", "\0x" and "x" all collide. Reachable here because
       the seed is folded into the offset basis, and a seed equal to that
       basis starts the state at exactly 0. One extra round costs nothing and
       removes the whole family. */
    h ^= len;
    h *= FNV_PRIME;
    return h;
}

} // namespace

namespace std {

/* libstdc++ passes its own seed here (0xc70f6907 by default); it is mixed in
   as FNV's offset basis so a caller-chosen seed still perturbs the result. */
size_t
_Hash_bytes(const void *ptr, size_t len, size_t seed)
{
    return fnv1a(ptr, len, seed ^ FNV_OFFSET);
}

size_t
_Fnv_hash_bytes(const void *ptr, size_t len, size_t seed)
{
    return fnv1a(ptr, len, seed ^ FNV_OFFSET);
}

} // namespace std
