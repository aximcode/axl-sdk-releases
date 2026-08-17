/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-arena-allocator.hpp
 *
 * #axl::arena_allocator — a standard Allocator backed by an #AxlArena, so a
 * standard container can be used on a path where a failed allocation must not
 * halt the image.
 *
 * @par The problem it solves
 *
 * `std::vector`, `std::string` and `std::map` are available unconditionally,
 * and they are the right default. What they cannot do is
 * participate in AXL's C error model. AXL treats out-of-memory as a value:
 * `axl_mem_fail_next_alloc()` is in a public header, the suite carries dozens
 * of OOM assertions, and some of them are degradation contracts rather than
 * error propagation. A standard container has nowhere to put that answer —
 * under `-fno-exceptions` its allocation failure lowers to a halt, and the
 * caller never gets a turn.
 *
 * An arena moves the failure. Its capacity is fixed at creation, so if
 * `axl_arena_new()` returned non-NULL and the arena is big enough for the
 * worst case, allocation from it cannot fail afterwards. Exhaustion stops
 * being an event scattered through the container's internals and becomes one
 * condition the caller checks up front, where it CAN degrade:
 *
 * @code
 * AXL_AUTOPTR(AxlArena) arena = axl_arena_new(64 * 1024);
 * if (arena == NULL) {
 *     return fall_back_to_the_lossy_path();     // caller's decision
 * }
 *
 * using ids = std::vector<uint32_t, axl::arena_allocator<uint32_t>>;
 * if (axl_arena_remaining(arena) < ids::allocator_type::bytes_for(n_max)) {
 *     return fall_back_to_the_lossy_path();     // still the caller's
 * }
 *
 * ids v{axl::arena_allocator<uint32_t>(arena)};
 * v.reserve(n_max);                             // cannot fail from here
 * @endcode
 *
 * @warning If you skip that check and the arena runs out, axl::arena_allocator::allocate() HALTS.
 *     It has no other option, and that is a property of the Allocator
 *     requirements rather than of this implementation: `allocate` must return
 *     valid storage or not return, and with `-fno-exceptions` the second is a
 *     halt. The check is the feature; the halt is the backstop.
 *
 * @par A custom allocator cannot make OOM recoverable
 *
 * It is reasonable to expect otherwise — containers are allocator-parameterised
 * precisely so callers can control allocation. But the interface has no way to
 * say "no". Returning NULL is not a refusal; it is accepted as success.
 * Measured, with an allocator whose `allocate` returns `nullptr`:
 *
 * @code
 * std::vector<int, null_alloc<int>> v;
 * v.reserve(4);      // SUCCEEDS: capacity() == 4, data() == nullptr
 * v.push_back(7);    // #PF, CR2 = 0 -- constructed through the null pointer
 * @endcode
 *
 * So the fault surfaces at the first *use*, not at the allocation, with
 * nothing pointing back at the allocator. What an allocator CAN change is
 * where memory comes from and when exhaustion becomes knowable — which is
 * exactly what this one does, by making capacity fixed and checkable before
 * the container exists.
 *
 * @par deallocate() does nothing — size for the PEAK, not the total
 *
 * An arena reclaims in bulk (`axl_arena_reset`) or not at all. Every
 * allocation an arena-backed container makes is permanent until then, and
 * that includes the ones it makes while GROWING. `push_back` to a capacity of
 * N without reserving first allocates 1, 2, 4 ... N and returns none of it,
 * so it consumes about 2N elements' worth of arena rather than N.
 *
 * `reserve()` up front is therefore not a performance note here, it is how
 * you make bytes_for() a true answer. bytes_for() describes ONE allocation of
 * @a n objects; a container that grows into it makes several.
 *
 * @par Application processors
 *
 * `axl_arena_alloc` is AP-safe — it is a lock-free CAS bump — while
 * `axl_malloc` goes through boot services, which do not exist on an
 * application processor. An arena-backed container is therefore usable from
 * MP-dispatched code where a default-allocator one is not. Creating,
 * resetting and freeing the arena stay BSP-only, so do those around the
 * dispatch, never inside it.
 *
 * @par Lifetime
 *
 * The allocator holds a borrowed pointer and never frees the arena. Destroy
 * every container before the arena: element destructors run over arena
 * memory, and for a non-trivial element type running them afterwards is a
 * use-after-free. Scoping the containers inside the arena's lifetime is
 * enough; `AXL_AUTOPTR(AxlArena)` at the enclosing scope gives exactly that
 * ordering.
 *
 * @warning `swap` and move-assignment MOVE THE ARENA WITH THE DATA. Both
 *     propagate (see the member typedefs), so after `a.swap(b)` the container
 *     `a` holds `b`'s buffer *and* `b`'s allocator — and now depends on `b`'s
 *     arena outliving it, not its own. That is what makes the operation
 *     well-defined at all, but it means "destroy the container before its
 *     arena" is a claim about the arena it holds NOW. Swapping between arenas
 *     with different lifetimes is where this bites.
 *
 * Needs no flag. The standard containers are available unconditionally since
 * T3 retired the freestanding C++ mode, and the allocator itself pulls in
 * nothing beyond `<memory>` and `<type_traits>`.
 */

#ifndef AXL_ARENA_ALLOCATOR_HPP
#define AXL_ARENA_ALLOCATOR_HPP

#ifndef __cplusplus
#error "axl-arena-allocator.hpp is C++ only; C consumers want axl_arena_alloc"
#endif

#include <stddef.h>
#include <stdint.h>

#include <type_traits>

#include <axl/axl-macros.h>
#include <axl/axl-signal.h>
#include <axl/axl-stream.h>
#include <axl/axl-task.h>

namespace axl {

/**
 * A standard Allocator that bump-allocates @a T out of an #AxlArena.
 *
 * Stateful: two allocators are equal exactly when they wrap the same arena.
 * See the file documentation for the exhaustion contract, the reason
 * `deallocate` is a no-op, and the lifetime rule.
 */
template <class T>
class arena_allocator {
public:
    using value_type = T;

    /* Both propagate, and neither is the default.
     *
     * SWAP is the one that matters for correctness: swapping two containers
     * whose allocators compare UNEQUAL is undefined behaviour unless the
     * allocator propagates, and two arena allocators are unequal whenever
     * they name different arenas -- which is the normal case, not the exotic
     * one. Leaving this at the default would make `std::swap(a, b)` UB for
     * ordinary code.
     *
     * MOVE ASSIGNMENT propagates so a move stays a move. Without it, move-
     * assigning between containers on different arenas silently degrades to
     * an element-by-element move into the destination's arena -- correct, but
     * it consumes arena space that the reader of `v = std::move(w);` has no
     * reason to expect.
     *
     * COPY assignment deliberately does NOT propagate: a copy should draw
     * from the destination's own arena, which is what the default gives. */
    using propagate_on_container_swap            = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;

    /**
     * Bind to @a arena. The arena is borrowed, never freed here, and must
     * outlive every container built on this allocator.
     */
    explicit
    arena_allocator(
        AxlArena *arena   ///< arena to allocate from; must outlive the container
    ) noexcept
        : m_arena(arena)
    {
    }

    /**
     * Rebinding conversion. Containers allocate their internal node types
     * through this, so it is reached far more often than the constructor
     * above.
     */
    template <class U>
    arena_allocator(
        const arena_allocator<U> &other   ///< allocator to share the arena of
    ) noexcept
        : m_arena(other.arena())
    {
    }

    /**
     * @brief Allocate storage for @a n objects of @a T.
     *
     * @warning HALTS if the arena cannot satisfy the request, or if @a n
     *     would overflow a byte count. Check bytes_for() against
     *     `axl_arena_remaining()` first on any path that must survive
     *     exhaustion.
     *
     * @return storage for @a n objects, suitably aligned and zeroed.
     */
    [[nodiscard]] T *
    allocate(
        size_t n   ///< number of objects
    )
    {
        size_t bytes = request_bytes(n);

        if (m_arena == nullptr) {
            halt("arena_allocator has no arena");
        }
        /* axl_arena_alloc treats 0 as an error and returns NULL, but
         * allocate(0) is legal and must not halt. One byte gives the
         * unique non-null pointer the standard expects. */
        if (bytes == 0) {
            bytes = 1;
        }
        void *p = axl_arena_alloc(m_arena, bytes);
        if (p == nullptr) {
            axl_printf("[axl] arena_allocator: arena exhausted -- wanted %zu of "
                       "%zu bytes remaining (capacity %zu)\r\n",
                       bytes, axl_arena_remaining(m_arena),
                       axl_arena_capacity(m_arena));
            halt("size the arena for the peak, or check "
                 "axl_arena_remaining() before committing");
        }
        return align_up(p);
    }

    /**
     * @brief Release storage — a no-op.
     *
     * An arena reclaims in bulk. See the file documentation: this is why an
     * arena-backed container must be sized for its PEAK footprint including
     * the intermediate buffers it allocates while growing.
     */
    void
    deallocate(
        T     *p,   ///< storage from #allocate (unused)
        size_t n    ///< object count (unused)
    ) noexcept
    {
        (void)p;
        (void)n;
    }

    /**
     * @brief Arena bytes an allocate() of @a n objects can consume.
     *
     * Two costs sit on top of `n * sizeof(T)`, and a caller sizing an arena
     * has no way to know either — which is why this exists instead of a
     * comment telling them to multiply:
     *
     * 1. padding to satisfy `alignof(T)`, where that exceeds the 8 bytes
     *    `axl_arena_alloc` guarantees;
     * 2. up to 7 bytes the arena spends rounding its own bump pointer up to 8
     *    before carving. Leaving this one out made the guard in the file
     *    documentation UNSOUND: with 3 bytes left, `remaining() >=
     *    bytes_for<char>(3)` passed and the allocate() after it halted.
     *
     * Describes ONE allocation. A container growing into capacity @a n makes
     * several and gets none of them back, so reserve up front.
     *
     * @return byte count, or `SIZE_MAX` if @a n cannot be represented (which
     *     makes any `remaining() < bytes_for(n)` check fail, as it should).
     */
    static size_t
    bytes_for(
        size_t n   ///< number of objects
    ) noexcept
    {
        const size_t req = request_bytes(n);

        if (req > SIZE_MAX - (arena_alignment() - 1)) {
            return SIZE_MAX;
        }
        return req + (arena_alignment() - 1);
    }

    /// The arena this allocator draws from.
    AxlArena *
    arena(void) const noexcept
    {
        return m_arena;
    }

private:
    /* axl_arena_alloc guarantees 8-byte alignment. Anything stricter is
     * padded for by hand -- silently under-aligning a type whose ABI needs 16
     * (long double, and every SIMD vector type) is the kind of defect that
     * shows up as a fault on one arch and works on the other.
     *
     * Functions rather than static constexpr members so nothing in this
     * header has static storage duration: a static in a header trips
     * bugprone-dynamic-static-initializers, which cannot see through the
     * template to prove constant initialization. */
    static constexpr size_t
    arena_alignment(void) noexcept
    {
        return 8;
    }

    static constexpr size_t
    align_slack(void) noexcept
    {
        return alignof(T) > arena_alignment() ? alignof(T) - arena_alignment() : 0;
    }

    /* What allocate() asks axl_arena_alloc for: the objects plus whatever
     * align_up may need to consume inside the block. Distinct from
     * bytes_for(), which additionally accounts for the arena's own
     * bump-pointer rounding OUTSIDE the block -- a caller checking
     * remaining() needs that, this does not. */
    static size_t
    request_bytes(
        size_t n   ///< number of objects
    ) noexcept
    {
        if (n > (SIZE_MAX - align_slack()) / sizeof(T)) {
            return SIZE_MAX;
        }
        return n * sizeof(T) + align_slack();
    }

    static T *
    align_up(
        void *p   ///< raw arena pointer
    ) noexcept
    {
        if (align_slack() == 0) {
            return static_cast<T *>(p);
        }
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        addr = (addr + (alignof(T) - 1)) & ~(uintptr_t)(alignof(T) - 1);
        return reinterpret_cast<T *>(addr);
    }

    AXL_NORETURN static void
    halt(
        const char *why   ///< what the caller should do differently
    )
    {
        axl_printf("[axl] arena_allocator: %s\r\n", why);
        axl_exit(1);
        __builtin_unreachable();
    }

    AxlArena *m_arena;
};

/// Two arena allocators are interchangeable exactly when they share an arena.
template <class T, class U>
inline bool
operator==(
    const arena_allocator<T> &a,   ///< left
    const arena_allocator<U> &b    ///< right
) noexcept
{
    return a.arena() == b.arena();
}

/// @see operator==(const arena_allocator<T> &, const arena_allocator<U> &)
template <class T, class U>
inline bool
operator!=(
    const arena_allocator<T> &a,   ///< left
    const arena_allocator<U> &b    ///< right
) noexcept
{
    return !(a == b);
}

} // namespace axl

#endif // AXL_ARENA_ALLOCATOR_HPP
