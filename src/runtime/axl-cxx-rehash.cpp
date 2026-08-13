/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-rehash.cpp
    AXL's own definition of the two out-of-line members of
    `std::__detail::_Prime_rehash_policy`, so `std::unordered_map`
    links without pulling `hashtable_c++0x.o` out of `libstdc++.a`.

    @par Why replace anything at all

    That one archive member is the only place libstdc++ does floating
    point in the container path — it computes the load factor. On a
    distribution whose gcc baseline is above plain `x86-64` (RHEL 10
    ships `-march=x86-64-v3`) the compiler renders that arithmetic in
    VEX-encoded AVX: 49 instructions of `vaddsd` / `vdivsd` /
    `vcvtsi2sd`. UEFI boots with `CR4.OSXSAVE` clear, so every one of
    them is `#UD - Invalid Opcode`.

    The failure is loud but misleading — `CR2` reads 0, which looks
    like a null-pointer dereference and is not one. `tree.o`, next to
    it in the same archive, is VEX-free purely because a red-black
    tree does no float math, which is why `std::map` runs on the same
    build where `std::unordered_map` faults.

    So the choice was between shipping a libstdc++ built to our
    baseline (a toolchain-build dependency), dropping
    `std::unordered_map` on x64 (a cross-arch hole in the SDK's
    contract), and supplying these two functions ourselves. They are
    small, fully specified by the comments on their own declarations,
    and compiled with the SDK's `$(GCC_ARCH)` — which is what makes
    them AVX-free by construction rather than by luck. `make
    check-no-avx` is the standing proof.

    Written from the declared contract, not derived from libstdc++'s
    implementation. The prime table is our own and deliberately not
    the same one.

    @par Both members or neither

    `_M_next_bkt` and `_M_need_rehash` share `_M_next_resize`. Taking
    one from here and the other from `libstdc++.a` links cleanly and
    then disagrees with itself about when to grow, so the definitions
    have to arrive as a pair. `test-cxx-hosted-qemu.sh` asserts with
    `ld -y` that BOTH resolved to `libaxl-cxx.a`, and exercises
    `reserve()` (which reaches `_M_next_bkt`) as well as plain
    insertion (which reaches `_M_need_rehash`).

    This implementation never trusts `_M_next_resize` for a decision;
    it recomputes from the arguments, which are complete on their own,
    and writes the cache only so `_M_state()` / `_M_reset()` stay
    meaningful. A memoized threshold that outlives a
    `max_load_factor()` change is the obvious bug here, and this
    cannot have it.

    @par ABI

    Compiled against the same `<bits/hashtable_policy.h>` that the
    consumer's `unordered_map` instantiates from, so the layout it
    assumes is by construction the layout in use. `_Prime_rehash_policy`
    is embedded by value in every `unordered_map`, which puts it inside
    the libstdc++ ABI that has been stable since GCC 5.

    Compiled `-fhosted` (see the Makefile's explicit rule) because
    `<unordered_map>` is unavailable otherwise — the very restriction
    `axl-c++ --hosted` exists to lift.
**/

#include <stddef.h>
#include <stdint.h>

#include <unordered_map>

#include <axl.h>

namespace {

/* Bucket counts. `_Mod_range_hashing` reduces a hash with `%
 * bucket_count`, and `std::hash<int>` is the identity, so a
 * power-of-two count would map any strided key sequence onto a
 * fraction of the table. Primes are what the policy's name promises.
 *
 * Growth is ~1.4x, so a rehash overshoots by at most that much while
 * still being geometric — which is what keeps insertion amortized
 * O(1). Every entry was Miller-Rabin verified when generated.
 */
const uint64_t k_bucket_primes[] = {
    2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 17ull, 23ull, 29ull, 37ull, 47ull,
    59ull, 73ull, 97ull, 137ull, 193ull, 271ull, 383ull, 541ull, 761ull,
    1069ull, 1499ull, 2099ull, 2939ull, 4127ull, 5779ull, 8093ull, 11351ull,
    15901ull, 22271ull, 31181ull, 43661ull, 61129ull, 85597ull, 119839ull,
    167777ull, 234893ull, 328883ull, 460451ull, 644647ull, 902507ull,
    1263511ull, 1768927ull, 2476511ull, 3467117ull, 4853983ull, 6795587ull,
    9513839ull, 13319431ull, 18647231ull, 26106131ull, 36548621ull,
    51168071ull, 71635303ull, 100289437ull, 140405297ull, 196567433ull,
    275194417ull, 385272187ull, 539381069ull, 755133497ull, 1057186937ull,
    1480061717ull, 2072086411ull, 2900920991ull, 4061289391ull,
    5685805171ull, 7960127261ull, 11144178169ull, 15601849441ull,
    21842589233ull, 30579624937ull, 42811474913ull, 59936064899ull,
    83910490907ull, 117474687293ull, 164464562233ull, 230250387137ull,
    322350542027ull, 451290758881ull, 631807062451ull, 884529887491ull,
    1238341842533ull, 1733678579621ull, 2427150011471ull, 3398010016067ull,
    4757214022531ull, 6660099631649ull, 9324139484311ull, 13053795278071ull,
    18275313389363ull, 25585438745117ull, 35819614243211ull,
    50147459940503ull, 70206443916743ull, 98289021483467ull,
    137604630076867ull, 192646482107669ull, 269705074950737ull,
    377587104931061ull, 528621946903507ull, 740070725664971ull,
    1036099015930963ull, 1450538622303499ull, 2030754071224909ull,
    2843055699714883ull, 3980277979600889ull, 5572389171441247ull,
    7801344840017759ull, 10921882776024863ull, 15290635886434847ull,
    21406890241008863ull, 29969646337412471ull, 41957504872377533ull,
    58740506821328597ull, 82236709549860059ull, 115131393369804127ull,
    161183950717725781ull, 225657531004816241ull, 315920543406742727ull,
    442288760769439841ull, 619204265077215799ull, 866885971108102103ull,
    1213640359551342851ull, 1699096503371879981ull, 2378735104720631809ull,
    3330229146608884301ull, 4662320805252438101ull, 6527249127353412617ull,
    9138148778294776843ull, 12793408289612687431ull,
    17910771605457760327ull,
};

const size_t k_bucket_prime_count =
    sizeof(k_bucket_primes) / sizeof(k_bucket_primes[0]);

/* Smallest tabulated prime >= @a n, or @a n itself once past the end
 * of the table. Above ~1.8e19 buckets the allocation cannot succeed
 * anyway, so a non-prime answer there costs distribution quality in a
 * case that does not occur rather than correctness. */
size_t
next_bucket_prime(
    size_t  n   ///< minimum acceptable bucket count
)
{
    size_t lo = 0;
    size_t hi = k_bucket_prime_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k_bucket_primes[mid] < (uint64_t) n) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == k_bucket_prime_count) {
        return n;
    }
    return (size_t) k_bucket_primes[lo];
}

/* A double that may be out of range, as a size_t. Also swallows NaN,
 * because the negated comparison is false for it. */
size_t
to_count(
    double  v   ///< value to clamp into [0, SIZE_MAX]
)
{
    if (!(v > 0.0)) {
        return 0;
    }
    if (!(v < (double) SIZE_MAX)) {
        return SIZE_MAX;
    }
    return (size_t) v;
}

/* The largest element count @a bkt buckets may hold before
 * `load_factor() <= max_load_factor()` stops holding. */
double
load_ceiling(
    size_t  bkt,   ///< bucket count
    float   mlf    ///< max load factor
)
{
    return (double) bkt * (double) mlf;
}

} // namespace

size_t
std::__detail::_Prime_rehash_policy::_M_next_bkt(size_t n) const
{
    /* "Return a bucket size no smaller than n" — and never 0 or 1,
     * because the caller reduces hashes modulo the result. */
    size_t bkt = next_bucket_prime(n < 2 ? 2 : n);

    _M_next_resize = to_count(load_ceiling(bkt, _M_max_load_factor));
    return bkt;
}

std::pair<bool, size_t>
std::__detail::_Prime_rehash_policy::_M_need_rehash(size_t n_bkt,
                                                    size_t n_elt,
                                                    size_t n_ins) const
{
    /* n_elt + n_ins is a count of live elements; it cannot overflow
     * without the allocations behind it having failed first. */
    const size_t need = n_elt + n_ins;

    if ((double) need <= load_ceiling(n_bkt, _M_max_load_factor)) {
        _M_next_resize = to_count(load_ceiling(n_bkt, _M_max_load_factor));
        return std::pair<bool, size_t>(false, 0);
    }

    /* Grow geometrically so a run of inserts stays amortized O(1),
     * but never by less than this insertion actually needs — a
     * reserve()-shaped call can ask for far more than 2x at once. */
    size_t want = n_bkt < SIZE_MAX / _S_growth_factor
                      ? n_bkt * _S_growth_factor
                      : SIZE_MAX;
    const size_t min_bkt = to_count(axl_ceil((double) need
                                             / (double) _M_max_load_factor));
    if (want < min_bkt) {
        want = min_bkt;
    }

    size_t bkt = next_bucket_prime(want < 2 ? 2 : want);

    /* min_bkt came through a double divide, so it can land one ulp
     * short of what the load factor actually requires. Step to the
     * next prime until it fits. This repairs rounding, it does not
     * search: one step is the realistic worst case, and the bound
     * makes termination unconditional rather than argued. */
    for (int guard = 0; guard < 8; guard++) {
        if ((double) need <= load_ceiling(bkt, _M_max_load_factor)) {
            break;
        }
        size_t next = next_bucket_prime(bkt + 1);
        if (next <= bkt) {
            break;   /* size_t exhausted; nothing larger exists */
        }
        bkt = next;
    }

    _M_next_resize = to_count(load_ceiling(bkt, _M_max_load_factor));
    return std::pair<bool, size_t>(true, bkt);
}
