/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-radix-tree.hpp
 *
 * `AxlRadixTree` with the payload type carried in the type system, ownership
 * carried by the destructor, and iteration through a lambda that can capture.
 *
 * @code
 * #include <axl/axl-radix-tree.hpp>
 *
 * axl::radix_tree<Route> routes;
 * routes.insert("/api/v1/", &v1);
 *
 * if (Route *r = routes.lookup("/api/v1/")) { ... }
 *
 * const char *tail = nullptr;                       // longest-prefix match
 * if (Route *r = routes.lookup_prefix("/api/v1/users/7", &tail)) { ... }
 *
 * int shown = 0;                                    // a CAPTURING visitor
 * routes.for_each([&](const char *key, Route *r) { if (r->visible) shown++; });
 * @endcode
 *
 * @par Three things C cannot do, which is the whole reason this exists
 *
 * AXL-Cxx-Design.md §6b's corollary is explicit that a C++ class forwarding to
 * a C API earns nothing, so this one is scoped to the parts where C++ has an
 * answer C does not:
 *
 * - **Ownership is the destructor.** The tree is freed on scope exit, on
 *   reassignment, and on the error path a hand-written `goto out` forgets.
 * - **The payload is typed.** `lookup()` returns `T *`; the `void *` round trip
 *   and its `static_cast` at every call site are gone, and storing the wrong
 *   type stops compiling instead of stopping at run time.
 * - **The visitor can capture.** axl_radix_tree_foreach() takes a function
 *   pointer plus a `void *`, so every real visitor is a hand-packed context
 *   struct. #axl::radix_tree::for_each() takes any callable, and the lambda inlines into the
 *   walk.
 *
 * Everything else is the C API, reachable through #axl::radix_tree::get() whenever something
 * here does not cover it. This type adds no data member beyond the handle.
 *
 * @par Move-only, because the C handle is
 *
 * `AxlRadixTree` has no refcount and AXL's ownership is a tree, not a graph —
 * the same reasoning `axl::unique_handle` records for having no shared form.
 * Copying is therefore deleted rather than deep-copying: a silent O(n) clone of
 * a routing table is not something a `=` should do.
 *
 * @par Who frees the values
 *
 * By default nothing does: the tree stores borrowed pointers, matching
 * axl_radix_tree_new(). Pass a destructor to the `AxlDestroyNotify`
 * constructor to have the tree own them, which is axl_radix_tree_new_full().
 * The choice is at construction and cannot change afterwards, exactly as in C.
 *
 * @par Allocation failure is visible, not thrown
 *
 * The constructor can fail — it allocates — and there are no exceptions here,
 * so it leaves the object EMPTY rather than halting. #axl::radix_tree::valid() reports it, and
 * every operation on an empty tree is a safe no-op returning the same thing it
 * would for a miss. #axl::radix_tree::insert() returns `false` on its own allocation failure.
 * That is AXL-Cxx-Design.md §6's "errors are values" applied to a constructor,
 * which cannot return one.
 */

#ifndef AXL_RADIX_TREE_HPP
#define AXL_RADIX_TREE_HPP

#ifndef __cplusplus
#error "axl-radix-tree.hpp is C++ only; C consumers want axl-radix-tree.h"
#endif

#include <cstddef>
#include <type_traits>
#include <utility>

#include <axl/axl-macros.h>
#include <axl/axl-radix-tree.h>

namespace axl {

/**
 * A prefix tree from string keys to `T *`, owning the C handle.
 *
 * @tparam T the pointee type stored against each key. The tree stores the
 *     pointer and never the object, so @a T may be incomplete here.
 */
template <class T>
class radix_tree {
public:
    /// A tree that BORROWS its values; nothing is freed on removal or
    /// destruction. axl_radix_tree_new().
    radix_tree() noexcept : m_tree(axl_radix_tree_new()) {}

    /**
     * A tree that OWNS its values, freeing each with @a destroy.
     *
     * axl_radix_tree_new_full(). @a destroy runs on the value a #remove()
     * discards, on the old value #axl::radix_tree::insert() replaces, and on every value left
     * at destruction.
     *
     * @a destroy is the C `AxlDestroyNotify` and takes `void *`, deliberately
     * rather than a typed `void (*)(T *)`. A typed parameter would read
     * better and can only be delivered to the C tree by
     * `reinterpret_cast`ing the function pointer — calling through a
     * different type than the function was defined with, which the standard
     * does not define however reliably it happens to work. There is nowhere
     * to hang a trampoline instead: the C tree calls the destructor with the
     * value alone and carries no user-data slot for it, unlike #axl::radix_tree::for_each().
     * So the one `static_cast` stays in the caller's destructor, where it is
     * visible.
     */
    explicit radix_tree(
        AxlDestroyNotify destroy    ///< value destructor, or NULL to borrow
    ) noexcept
        : m_tree(axl_radix_tree_new_full(destroy))
    {}

    ~radix_tree() { axl_radix_tree_free(m_tree); }

    radix_tree(const radix_tree &) = delete;
    radix_tree &operator=(const radix_tree &) = delete;

    radix_tree(radix_tree &&other) noexcept
        : m_tree(std::exchange(other.m_tree, nullptr))
    {}

    radix_tree &
    operator=(radix_tree &&other) noexcept
    {
        if (this != &other) {
            axl_radix_tree_free(m_tree);
            m_tree = std::exchange(other.m_tree, nullptr);
        }
        return *this;
    }

    /**
     * Whether construction succeeded.
     *
     * False after an allocation failure in the constructor, and after this
     * tree has been moved from. Every other member is a safe no-op in that
     * state, so checking is a choice rather than a precondition.
     */
    [[nodiscard]] bool valid() const noexcept { return m_tree != nullptr; }

    /// @copydoc valid()
    explicit operator bool() const noexcept { return valid(); }

    /**
     * Insert or replace @a key's value.
     *
     * @a key is COPIED into the tree; @a value is not. Replacing an existing
     * key frees the old value if this tree owns its values.
     *
     * A NULL @a value is storable and behaves like any other entry: counted
     * once by #size(), visited by #for_each(), removable by #remove(). Only
     * #lookup() cannot distinguish it from an absent key, because its return
     * type has no spare value to say so.
     *
     * @return true on success; false on allocation failure, on an invalid
     *     tree, or on a NULL @a key. axl_radix_tree_insert()'s `AXL_OK`, as a
     *     bool — the C call reports failure as `AXL_ERR` and this is the only
     *     outcome a caller can act on.
     */
    bool
    insert(
        const char *key,     ///< key to insert under, copied
        T          *value    ///< value to store, borrowed unless owned
    ) noexcept
    {
        return axl_radix_tree_insert(m_tree, key, value) == AXL_OK;
    }

    /**
     * Exact lookup.
     *
     * @return the stored value, or NULL if @a key is absent. A key stored
     *     against a NULL value is indistinguishable from an absent one, which
     *     is the C API's behaviour and not a narrowing.
     */
    [[nodiscard]] T *
    lookup(
        const char *key    ///< key to find
    ) const noexcept
    {
        return static_cast<T *>(axl_radix_tree_lookup(m_tree, key));
    }

    /**
     * Longest-prefix lookup: the value of the longest stored key that is a
     * prefix of @a key.
     *
     * @a suffix receives a pointer INTO @a key, at the first character past
     * the matched prefix — so the matched length is `*suffix - key`, and the
     * remainder is a valid C string because it points into @a key's own bytes.
     * Untouched when nothing matches. Pass NULL when the remainder is not
     * wanted.
     *
     * @return the value of the longest matching prefix, or NULL if no stored
     *     key is a prefix of @a key.
     */
    [[nodiscard]] T *
    lookup_prefix(
        const char  *key,             ///< key to match against
        const char **suffix = nullptr ///< [out] rest of @a key past the match
    ) const noexcept
    {
        return static_cast<T *>(
            axl_radix_tree_lookup_prefix(m_tree, key, suffix));
    }

    /**
     * Remove @a key, freeing its value if this tree owns its values.
     *
     * @return true if @a key was present and removed; false if it was absent.
     */
    bool
    remove(
        const char *key    ///< key to remove
    ) noexcept
    {
        return axl_radix_tree_remove(m_tree, key);
    }

    /**
     * @return the number of entries; 0 for an invalid tree.
     *
     * Counts KEYS, including any whose value is NULL, and every key it counts
     * is reachable by #remove() and #for_each().
     */
    [[nodiscard]] size_t size() const noexcept
    {
        return axl_radix_tree_size(m_tree);
    }

    /// @return true when #size() is 0, which includes an invalid tree.
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /**
     * Visit every entry, depth-first, with a callable that may capture.
     *
     * @a visit is invoked as `visit(const char *key, T *value)`. The key is
     * reconstructed for the call and is valid only for its duration — copy it
     * to keep it. The tree must not be modified during the walk.
     *
     * @tparam F any callable of that signature. It must not throw: the walk
     *     crosses the C library, where an exception has no frame table to
     *     unwind through, so the trampoline below is `noexcept` and a throw
     *     reaching it terminates loudly rather than corrupting the walk. This
     *     is #AXL_CB_NOEXCEPT's rule, enforced by the compiler rather than
     *     documented.
     *
     * @warning This is the one member that can fail SILENTLY. The C walk
     *     rebuilds each key into a heap buffer, and if that allocation fails
     *     it returns without visiting — mid-walk, skipping an entire subtree,
     *     with no report. So a visit count below #size() means exhaustion,
     *     not an empty tree. Everything else here reports failure as a value.
     */
    template <class F>
    void
    for_each(
        F &&visit    ///< callable(const char *key, T *value)
    ) const noexcept
    {
        /* The trampoline is a capture-less lambda so it converts to the C
           function pointer with no cast between function types; the real
           callable rides in the void * the C API already carries for exactly
           this purpose.

           Its key parameter is `const void *`, which is what
           AxlHashTableForeachFunc declares -- the radix tree reuses the hash
           table's visitor type, and taking `const char *` here would be a
           signature mismatch the conversion silently would not make. */
        /* @a visit is reached through a shim that CAPTURES IT BY REFERENCE,
           which handles the three callable spellings uniformly and is the
           reason this is not simply `&visit`:

           - a plain function NAME deduces F as `void(&)(const char *, T *)`,
             leaving a FUNCTION type whose address cannot be cast to `void *`
             at all — so `t.for_each(my_visitor)` failed to compile INSIDE
             this header while `t.for_each(&my_visitor)` worked;
           - a `mutable` lambda must see its own captures mutated, so copying
             the callable (the other obvious fix) would silently discard
             everything it accumulated.

           The shim is an object, so its address is always a valid `void *`,
           and by-reference capture keeps the caller's object authoritative. */
        auto shim = [&visit](const char *k, T *v) AXL_CB_NOEXCEPT {
            visit(k, v);
        };
        using Fn = decltype(shim);
        AxlHashTableForeachFunc trampoline =
            [](const void *key, void *value, void *data) AXL_CB_NOEXCEPT {
                (*static_cast<Fn *>(data))(static_cast<const char *>(key),
                                           static_cast<T *>(value));
            };
        axl_radix_tree_foreach(
            m_tree, trampoline,
            const_cast<void *>(static_cast<const void *>(&shim)));
    }

    /**
     * The underlying C handle, still owned by this object.
     *
     * For the C calls this class does not wrap. Do not free it; use #release()
     * to take ownership away.
     *
     * @return the handle, or NULL if invalid.
     */
    [[nodiscard]] AxlRadixTree *get() const noexcept { return m_tree; }

    /**
     * Give up ownership: return the handle and become invalid.
     *
     * @return the handle, now the caller's to axl_radix_tree_free().
     */
    [[nodiscard]] AxlRadixTree *
    release() noexcept
    {
        return std::exchange(m_tree, nullptr);
    }

private:
    AxlRadixTree *m_tree;
};

} // namespace axl

#endif /* AXL_RADIX_TREE_HPP */
