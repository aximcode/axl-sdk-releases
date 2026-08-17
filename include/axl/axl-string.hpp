/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-string.hpp
 *
 * #axl::string — an owning, growing string for FREESTANDING C++, with
 * `std::string`'s interface and a small-string optimisation.
 *
 * @par Why this exists when axl-cxx.hpp says it should not
 *
 * That file argues there is no `axl::string` because `std::string` works.
 * This class was originally written because that held only conditionally:
 * `<string>` was gated by `bits/requires_hosted.h`, so a freestanding
 * translation unit had no owning string at all. **That reason is gone** — T3
 * retired the freestanding C++ mode, and `std::string` is now available
 * everywhere.
 *
 * It is kept for a different reason, and the right one: **out-of-memory is a
 * value here.** `std::string` has nowhere to put an allocation failure. Under
 * `-fno-exceptions` it halts the image, and `operator new` may not soften that
 * by returning NULL because libstdc++ hands the result to the container
 * unchecked. This class sets a sticky #axl::string::bad() and leaves the
 * contents untouched, which is the same contract `axl_mem_fail_next_alloc()`
 * and the suite's OOM assertions are built on.
 *
 * That is not a preference — #axl::istream depends on it. `axl::cin >> s`
 * reports an accumulation OOM as `AXL_NO_RESOURCES` by reading
 * #axl::string::bad() off its accumulator; with `std::string` the halt would
 * happen below the stream, with no value left to report.
 *
 * It is also the cheaper of the two: measured on x64 `--release`, an
 * equivalent construct-append-grow program costs **564 bytes** with this
 * class and **1045 bytes** with `std::string`.
 *
 * **So: prefer `std::string`** on any path that can pre-size or that may
 * legitimately halt — it is the one every other C++ programmer knows, and
 * #axl::arena_allocator gives it a pre-checked capacity where that fits.
 * Reach for this class when a path must SURVIVE exhaustion and cannot know
 * the size up front, which is exactly the case `axl::cin >> s` presents.
 *
 * @par Why it is NOT a skin over AxlString
 *
 * It was, briefly, and `AXL-Cxx-Design.md` §4.5 had already measured why that
 * is wrong. Titled "GO for the structure, NO-GO for the skin", N=200000, warm
 * heap, microseconds:
 *
 * | shape                  | ctor (short) | copy (short) |
 * |------------------------|-------------:|-------------:|
 * | skin over `AxlString`  |        29055 |        29336 |
 * | standalone, always heap|        13749 |        14481 |
 * | **standalone, SSO**    |     **3142** |     **3034** |
 *
 * **9.2x on constructing a short string and 9.7x on copying one** — the two
 * operations firmware does most, on the strings it actually holds. The control
 * is `ctor LONG`, which forces an allocation in every shape and converges at
 * ~300 ms for all four: proof that the whole difference is allocation
 * AVOIDANCE, which a skin cannot express. `AxlString`'s handle is a pointer to
 * a heap object, so there is nowhere to put the inline bytes.
 *
 * Confirmed on THIS implementation rather than inherited: the same benchmark
 * before and after the change, one machine, N=200000, `\EFI\BOOT\BOOTX64.EFI`
 * (21 bytes), warm heap — ctor 33279 -> 4790 us (**6.9x**), copy
 * 31120 -> 3324 us (**9.4x**). Treat the ratios as the durable part; the
 * absolute microseconds are QEMU's.
 *
 * `AxlString` keeps the job it is good at, where §4.5 measured it TIED:
 * the streaming builder behind the JSON and XML writer sinks.
 *
 * @par The interface is std::string's, deliberately
 *
 * Every member below means what `std::string`'s member of that name means,
 * so porting hosted code stays `s/std::/axl::/` — the same property that
 * makes the stream objects #axl::cout rather than `axl::out`. The search
 * family (`find`, `rfind`, `find_first_of`, `substr`, `compare`,
 * `starts_with`, ...) is FORWARDED to `std::string_view`, which is in the
 * freestanding subset. Those are not reimplementations that might drift; they
 * are libstdc++'s own algorithms reading our bytes.
 *
 * @par Two deliberate divergences
 *
 * **Out-of-memory sets #axl::string::bad() instead of halting.** `std::string` has nowhere
 * to put an allocation failure — under `-fno-exceptions` it lowers to a halt
 * and the caller never gets a turn. A mutator here that cannot get memory
 * leaves the string UNCHANGED, sets a sticky #axl::string::bad() flag, and returns. That
 * is AXL's model (`axl_mem_fail_next_alloc()` is public, and the suite treats
 * exhaustion as a value), and it is what lets `axl::cin >> s` fold an OOM
 * into the stream's fail state instead of taking the image down.
 *
 * **#axl::string::at() still halts.** Not an inconsistency: an out-of-range index is a
 * program bug with no valid recovery, while out-of-memory is an environmental
 * condition firmware is expected to survive. `std::string::at` is the checked
 * accessor whose whole contract is that it does not return on a bad index, so
 * it reaches the same `std::__throw_out_of_range_fmt` halt every other
 * `-fno-exceptions` throw site in this SDK reaches.
 *
 * @par Where this is more forgiving than std::string
 *
 * An out-of-range position is CLAMPED rather than fatal, for every member
 * that takes one except #axl::string::at():
 *
 * | Call on `"abc"`        | here      | `std::string`  |
 * |------------------------|-----------|----------------|
 * | `s.insert(99, "Z")`    | appends   | `out_of_range` |
 * | `s.substr(99)`         | `""`      | `out_of_range` |
 * | `s.erase(99)`          | no-op     | `out_of_range` |
 * | `s.replace(99, 1, v)`  | appends   | `out_of_range` |
 *
 * `at()` is the exception BECAUSE it is the member whose entire purpose is to
 * be the checked one — a caller who wanted clamping would have written
 * `operator[]`. Everywhere else, halting a firmware image over an arithmetic
 * slip is a worse outcome than clamping, and there is no third option under
 * `-fno-exceptions`.
 *
 * @par Cost
 *
 * 48 bytes per string, against libstdc++'s 32 — a pointer, a size, the
 * 24-byte inline buffer (whose space the heap capacity reuses) and the sticky
 * OOM flag. No MUTATOR allocates at or below #axl::string::sso_capacity, so a
 * short string cannot fail; #axl::string::steal() is the one exception, since
 * it has to hand back a heap block whatever the content.
 *
 * The pointer is ALWAYS valid, pointing at the inline buffer exactly while
 * the content is inline. Reads therefore never branch on which mode the
 * string is in, and there is no empty-string special case — which is how
 * `data()` and `c_str()` can promise to be the same pointer.
 *
 * @par The one place bad() does not follow the value
 *
 * The copy constructor does NOT inherit #axl::string::bad(): a copy of a
 * truncated string reports itself healthy. That is deliberate — the flag
 * records that THIS object's mutation failed, not that its bytes are suspect
 * — but it does mean passing a string by value launders the flag. Check
 * #axl::string::bad() where the mutation happens, not downstream.
 *
 * @code
 * axl::string host;
 * uint16_t    port;
 *
 * axl::cout << "target: ";
 * axl::cin >> host >> port;
 * if (!axl::cin) {
 *     return axl::err(AXL_INVALID);
 * }
 *
 * if (host.starts_with("http://")) {          // std::string_view's own
 *     host.erase(0, 7);
 * }
 * axl::cout << host << ':' << port << axl::endl;
 * @endcode
 *
 * @see axl-cxx.hpp for the error vocabulary, axl-ostream.hpp and
 *     axl-istream.hpp for the streams this was built to serve.
 */

#ifndef AXL_STRING_HPP
#define AXL_STRING_HPP

#ifndef __cplusplus
#error "axl-string.hpp is C++ only; C consumers want axl/axl-string.h"
#endif

#include <compare>
#include <iterator>
#include <string_view>

#include <bits/functexcept.h>   // std::__throw_out_of_range_fmt (see at())

#include <axl/axl-cxx.hpp>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

namespace axl {

/**
 * An owning, growing UTF-8 string with `std::string`'s interface, usable in a
 * freestanding translation unit.
 *
 * See the file documentation for why this exists alongside `std::string`, for
 * the small-string optimisation that makes it worth having, and for the two
 * places it deliberately behaves differently (out-of-memory sets
 * #axl::string::bad(); #axl::string::at() halts).
 */
class string {
public:
    using value_type      = char;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = char &;
    using const_reference = const char &;
    using pointer         = char *;
    using const_pointer   = const char *;
    using iterator        = char *;
    using const_iterator  = const char *;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    /// "Not found" / "to the end", as `std::string::npos`.
    static constexpr size_type npos = static_cast<size_type>(-1);

    /**
     * Bytes a string holds before it needs the heap at all.
     *
     * 23 rather than `std::string`'s 15, chosen from what firmware actually
     * holds: `\EFI\BOOT\BOOTX64.EFI` is 21 characters, and a 15-byte inline
     * buffer would spill the single most common path in the tree onto the
     * heap. The cost is 48 bytes per string against libstdc++'s 32.
     */
    static constexpr size_type sso_capacity = 23;

    // -----------------------------------------------------------------
    // Construction and lifetime
    // -----------------------------------------------------------------

    /// An empty string. Allocates nothing, so it cannot fail.
    constexpr string() noexcept
        : m_ptr(m_sso), m_size(0), m_sso{}, m_bad(false)
    {
    }

    /// A copy of the NUL-terminated @a s. A NULL @a s gives an empty string.
    string(const char *s) : string()
    {
        if (s != nullptr) {
            append(s, axl_strlen(s));
        }
    }

    /// A copy of @a n bytes from @a s, which may contain embedded NULs.
    string(const char *s, size_type n) : string()
    {
        append(s, n);
    }

    /**
     * A copy of @a v.
     *
     * `explicit`, as `std::string`'s `string_view` constructor is. Without
     * that, `some_view == "literal"` becomes ambiguous inside namespace
     * `axl`: the view would convert to a #axl::string and pick up the
     * `operator==(const string &, const char *)` overload below as a second
     * candidate.
     */
    explicit string(std::string_view v) : string()
    {
        append(v.data(), v.size());
    }

    /// @a n copies of @a c.
    string(size_type n, char c) : string()
    {
        append(n, c);
    }

    /// A copy of @a other's contents. #axl::string::bad() is NOT inherited.
    string(const string &other) : string()
    {
        append(other.m_ptr, other.m_size);
    }

    /**
     * Takes @a other's buffer, leaving it empty and usable.
     *
     * A SHORT string is copied rather than stolen: its bytes live inside the
     * object, so there is no buffer to take. That is inherent to any SSO
     * string, `std::string` included — and it is 23 bytes, not an allocation.
     */
    string(string &&other) noexcept : string()
    {
        adopt(other);
    }

    ~string()
    {
        if (!local()) {
            axl_free(m_ptr);
        }
    }

    /// Replaces the contents AND the #axl::string::bad() state.
    string &operator=(const string &other)
    {
        if (this != &other) {
            assign(other.m_ptr, other.m_size);
        }
        return *this;
    }

    /// Takes @a other's buffer, leaving it empty and usable.
    string &operator=(string &&other) noexcept
    {
        if (this != &other) {
            if (!local()) {
                axl_free(m_ptr);
            }
            m_ptr  = m_sso;
            m_size = 0;
            adopt(other);
        }
        return *this;
    }

    string &operator=(const char *s)       { return assign(s); }
    string &operator=(std::string_view v)  { return assign(v.data(), v.size()); }
    string &operator=(char c)              { return assign(1, c); }

    // -----------------------------------------------------------------
    // Conversion
    // -----------------------------------------------------------------

    /// A view of the bytes. Invalidated by anything that can reallocate.
    operator std::string_view() const noexcept
    {
        return {m_ptr, m_size};
    }

    /// Same as the conversion, spelled out for where deduction cannot help.
    std::string_view view() const noexcept
    {
        return {m_ptr, m_size};
    }

    // -----------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------

    size_type size() const noexcept   { return m_size; }
    size_type length() const noexcept { return m_size; }
    bool      empty() const noexcept  { return m_size == 0; }

    /// Bytes of content this can hold before it must grow.
    size_type capacity() const noexcept
    {
        return local() ? sso_capacity : m_cap;
    }

    /// The largest string that could be represented, as `std::string`'s.
    static constexpr size_type max_size() noexcept
    {
        return static_cast<size_type>(-1) / 2;
    }

    /// Make room for @a n bytes. Sets #axl::string::bad() and does nothing on failure.
    void reserve(size_type n) { (void)grow_to(n); }

    /**
     * Release unused capacity.
     *
     * Returns to the inline buffer entirely when the content fits, so a
     * string that was briefly long stops costing a heap block at all.
     * A failed shrink is not an error — the larger buffer is kept and the
     * string stays usable, the same latitude `std::string` has.
     */
    void shrink_to_fit() noexcept
    {
        if (local() || m_cap == m_size) {
            return;
        }
        if (m_size <= sso_capacity) {
            char *old = m_ptr;
            axl_memcpy(m_sso, old, m_size + 1);
            m_ptr = m_sso;
            axl_free(old);
            return;
        }
        char *nb = (char *)axl_realloc(m_ptr, m_size + 1);
        if (nb != nullptr) {
            m_ptr = nb;
            m_cap = m_size;
        }
    }

    /// Set the length to @a n, padding with @a c when growing.
    void resize(size_type n, char c)
    {
        if (n <= m_size) {
            m_size = n;
            m_ptr[m_size] = '\0';
            return;
        }
        size_type add = n - m_size;
        if (!grow_to(n)) {
            return;
        }
        axl_memset(m_ptr + m_size, c, add);
        m_size = n;
        m_ptr[m_size] = '\0';
    }

    /// Set the length to @a n, padding with NUL bytes when growing.
    void resize(size_type n) { resize(n, '\0'); }

    /// Empty the contents. Keeps the capacity, and keeps #axl::string::bad().
    void clear() noexcept
    {
        m_size = 0;
        m_ptr[0] = '\0';
    }

    // -----------------------------------------------------------------
    // Element access
    // -----------------------------------------------------------------

    /// The byte at @a pos. Out of range is undefined, as `std::string`'s.
    reference       operator[](size_type pos)       { return m_ptr[pos]; }
    const_reference operator[](size_type pos) const { return m_ptr[pos]; }

    /**
     * The byte at @a pos, bounds-checked.
     *
     * @warning HALTS on an out-of-range @a pos, through the same
     *     `std::__throw_out_of_range_fmt` every `-fno-exceptions` throw site
     *     in this SDK reaches. That is `std::string::at`'s contract; a bad
     *     index is a program bug, not a condition to degrade around. Use
     *     #operator[] after your own check, or #view() plus the
     *     `std::string_view` API, on a path that must not halt.
     */
    reference at(size_type pos)
    {
        if (pos >= m_size) {
            std::__throw_out_of_range_fmt(
                "axl::string::at: pos (which is %zu) >= size() (which is %zu)",
                pos, m_size);
        }
        return m_ptr[pos];
    }

    /// @copydoc at()
    const_reference at(size_type pos) const
    {
        if (pos >= m_size) {
            std::__throw_out_of_range_fmt(
                "axl::string::at: pos (which is %zu) >= size() (which is %zu)",
                pos, m_size);
        }
        return m_ptr[pos];
    }

    reference       front()       { return m_ptr[0]; }
    const_reference front() const { return m_ptr[0]; }
    reference       back()        { return m_ptr[m_size - 1]; }
    const_reference back() const  { return m_ptr[m_size - 1]; }

    /**
     * The bytes, NUL-terminated. Never NULL, even for an empty string.
     *
     * `data()` and `c_str()` return the SAME pointer, and `begin()` equals
     * `cbegin()`, as `std::string` guarantees — the buffer is always real,
     * inline when short, so there is no empty-string special case to get
     * wrong.
     */
    const char *c_str() const noexcept { return m_ptr; }

    /// @copydoc c_str()
    const char *data() const noexcept { return m_ptr; }

    /**
     * The bytes, writable and NUL-terminated. Never NULL.
     *
     * Writing changes the contents but never the length; only the first
     * #size() bytes may be written, exactly as for `std::string::data`.
     */
    char *data() noexcept { return m_ptr; }

    // -----------------------------------------------------------------
    // Iterators
    // -----------------------------------------------------------------

    iterator       begin() noexcept        { return m_ptr; }
    iterator       end() noexcept          { return m_ptr + m_size; }
    const_iterator begin() const noexcept  { return m_ptr; }
    const_iterator end() const noexcept    { return m_ptr + m_size; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept   { return end(); }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept   { return reverse_iterator(begin()); }

    const_reverse_iterator rbegin() const noexcept
    {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator rend() const noexcept
    {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    const_reverse_iterator crend() const noexcept   { return rend(); }

    // -----------------------------------------------------------------
    // Modifiers
    // -----------------------------------------------------------------

    /**
     * Append @a n bytes from @a s. Sets #axl::string::bad() and changes
     * nothing on OOM.
     *
     * @a s may point into this string's own buffer (`s += s`). Growth can
     * move that buffer, so the source is re-anchored by offset across the
     * reallocation — no temporary is needed here, because the destination
     * starts at #size() and cannot overlap a source that ends there.
     */
    string &append(const char *s, size_type n)
    {
        if (s == nullptr || n == 0) {
            return *this;
        }

        bool      self = aliases_ptr(s);
        size_type off  = self ? static_cast<size_type>(s - m_ptr) : 0;

        if (!grow_to_add(n)) {
            return *this;
        }

        axl_memcpy(m_ptr + m_size, self ? m_ptr + off : s, n);
        m_size += n;
        m_ptr[m_size] = '\0';
        return *this;
    }

    string &append(const char *s)
    {
        return s != nullptr ? append(s, axl_strlen(s)) : *this;
    }
    string &append(std::string_view v)   { return append(v.data(), v.size()); }
    string &append(const string &o)      { return append(o.m_ptr, o.m_size); }

    /// Append @a n copies of @a c.
    string &append(size_type n, char c)
    {
        if (n == 0) {
            return *this;
        }
        if (!grow_to_add(n)) {
            return *this;
        }
        axl_memset(m_ptr + m_size, c, n);
        m_size += n;
        m_ptr[m_size] = '\0';
        return *this;
    }

    string &operator+=(const char *s)      { return append(s); }
    string &operator+=(std::string_view v) { return append(v); }
    string &operator+=(const string &o)    { return append(o); }
    string &operator+=(char c)             { return append(1, c); }

    /**
     * Replace the contents with @a n bytes from @a s. Resets
     * #axl::string::bad().
     *
     * Appends FIRST and drops the old bytes afterwards, so that a @a s
     * pointing into this string's own buffer (`s = s.c_str()`) still reads
     * intact bytes. Clearing first would write a NUL over byte 0 and then
     * copy from that same buffer, turning `"hello"` into `"\0ello"`.
     */
    string &assign(const char *s, size_type n)
    {
        m_bad = false;

        /* The append-then-erase dance is ONLY for a self-aliasing source. Done
           unconditionally it makes the peak length `old + n` when the result is
           just `n` -- so assigning "ab" over a 22-byte string ALLOCATED, and
           could fail, for a result that fits inline twice over. That falsified
           this file's own "nothing at or below sso_capacity can fail", and it
           meant copy-ASSIGNMENT allocated where copy-CONSTRUCTION did not. */
        if (!aliases_ptr(s)) {
            clear();
            return append(s, n);
        }

        size_type old = m_size;
        append(s, n);
        if (!m_bad) {
            erase(0, old);
        }
        return *this;
    }

    string &assign(const char *s)
    {
        return s != nullptr ? assign(s, axl_strlen(s)) : assign("", 0);
    }
    string &assign(std::string_view v) { return assign(v.data(), v.size()); }
    string &assign(const string &o)    { return assign(o.m_ptr, o.m_size); }

    /// Replace the contents with @a n copies of @a c.
    string &assign(size_type n, char c)
    {
        clear();
        m_bad = false;
        return append(n, c);
    }

    void push_back(char c) { append(1, c); }

    /// Remove the last byte. A no-op on an empty string.
    void pop_back()
    {
        if (m_size != 0) {
            m_size--;
            m_ptr[m_size] = '\0';
        }
    }

    /**
     * Insert @a n bytes from @a s at @a pos.
     *
     * A @a s inside this string's own buffer is copied out first: the shift
     * below shuffles the bytes, and a source that straddles @a pos would be
     * half-moved. The copy is free for a short source, which is the case
     * that actually occurs.
     */
    string &insert(size_type pos, const char *s, size_type n)
    {
        if (s == nullptr || n == 0) {
            return *this;
        }
        if (pos > m_size) {
            pos = m_size;
        }
        if (aliases_ptr(s)) {
            string tmp(s, n);
            if (tmp.m_bad) {
                m_bad = true;
                return *this;
            }
            return insert_disjoint(pos, tmp.m_ptr, n);
        }
        return insert_disjoint(pos, s, n);
    }

    string &insert(size_type pos, const char *s)
    {
        return s != nullptr ? insert(pos, s, axl_strlen(s)) : *this;
    }
    string &insert(size_type pos, std::string_view v)
    {
        return insert(pos, v.data(), v.size());
    }
    string &insert(size_type pos, const string &o)
    {
        return insert(pos, o.m_ptr, o.m_size);
    }

    /// Remove @a n bytes at @a pos, or to the end when @a n is #npos.
    string &erase(size_type pos = 0, size_type n = npos)
    {
        if (pos >= m_size || n == 0) {
            return *this;
        }
        size_type cut = (n == npos || n > m_size - pos) ? m_size - pos : n;

        axl_memmove(m_ptr + pos, m_ptr + pos + cut, m_size - pos - cut + 1);
        m_size -= cut;
        return *this;
    }

    /**
     * Replace the @a n bytes at @a pos with @a v.
     *
     * The one COMPOUND mutator here, so it is the one that has to work to
     * keep the file's "unchanged on OOM" promise: the capacity for the final
     * length is taken up front, so neither the erase nor the insert can fail
     * for memory afterwards. Without that, an OOM between the two halves
     * leaves @a n bytes destroyed and nothing put back.
     *
     * A @a v pointing into this string's own buffer (`s.replace(0, 2, s)`) is
     * copied out first — the erase shifts the bytes out from under it, and
     * `v.size()` was captured before that happened.
     */
    string &replace(size_type pos, size_type n, std::string_view v)
    {
        if (pos > m_size) {
            pos = m_size;
        }
        if (n > m_size - pos) {
            n = m_size - pos;
        }

        if (aliases_ptr(v.data()) && !v.empty()) {
            string tmp(v.data(), v.size());
            if (tmp.m_bad) {
                m_bad = true;
                return *this;
            }
            return replace_disjoint(pos, n, tmp.m_ptr, tmp.m_size);
        }
        return replace_disjoint(pos, n, v.data(), v.size());
    }

    string &replace(size_type pos, size_type n, const char *s)
    {
        return replace(pos, n, std::string_view(s != nullptr ? s : ""));
    }

    /// Exchange contents with @a other. Cannot fail.
    void swap(string &other) noexcept
    {
        string tmp(static_cast<string &&>(*this));
        *this = static_cast<string &&>(other);
        other = static_cast<string &&>(tmp);
    }

    /// A copy of the @a n bytes at @a pos. Check #axl::string::bad() on the result.
    string substr(size_type pos = 0, size_type n = npos) const
    {
        std::string_view v = view();
        if (pos > v.size()) {
            return string{};
        }
        return string(v.substr(pos, n));
    }

    // -----------------------------------------------------------------
    // Search and compare -- forwarded to std::string_view, so these are
    // libstdc++'s own algorithms rather than reimplementations that drift.
    // -----------------------------------------------------------------

    size_type find(std::string_view v, size_type pos = 0) const noexcept
    {
        return view().find(v, pos);
    }
    size_type find(char c, size_type pos = 0) const noexcept
    {
        return view().find(c, pos);
    }
    size_type rfind(std::string_view v, size_type pos = npos) const noexcept
    {
        return view().rfind(v, pos);
    }
    size_type rfind(char c, size_type pos = npos) const noexcept
    {
        return view().rfind(c, pos);
    }
    size_type find_first_of(std::string_view v, size_type pos = 0) const noexcept
    {
        return view().find_first_of(v, pos);
    }
    size_type find_last_of(std::string_view v, size_type pos = npos) const noexcept
    {
        return view().find_last_of(v, pos);
    }
    size_type find_first_not_of(std::string_view v, size_type pos = 0) const noexcept
    {
        return view().find_first_not_of(v, pos);
    }
    size_type find_last_not_of(std::string_view v, size_type pos = npos) const noexcept
    {
        return view().find_last_not_of(v, pos);
    }

    int  compare(std::string_view v) const noexcept { return view().compare(v); }
    bool starts_with(std::string_view v) const noexcept
    {
        return view().starts_with(v);
    }
    bool starts_with(char c) const noexcept { return view().starts_with(c); }
    bool ends_with(std::string_view v) const noexcept
    {
        return view().ends_with(v);
    }
    bool ends_with(char c) const noexcept { return view().ends_with(c); }
    bool contains(std::string_view v) const noexcept { return view().contains(v); }
    bool contains(char c) const noexcept { return view().contains(c); }

    // -----------------------------------------------------------------
    // Error state
    // -----------------------------------------------------------------

    /**
     * Did any mutation fail for want of memory?
     *
     * Sticky for the lifetime of the value: once set it stays set until the
     * string is #assign()ed or assigned a new value. A #axl::string::bad()
     * string is still VALID and usable — the failed operation left the
     * contents alone, so what it holds is the last state that fit in memory.
     *
     * A string that never exceeds #sso_capacity cannot set this through any
     * mutator, because none of them allocates. #axl::string::steal() is the
     * single exception: it must produce a heap block, so it allocates even
     * for inline content.
     */
    bool bad() const noexcept { return m_bad; }

    /// Is the content held inline, with no heap block behind it?
    bool is_small() const noexcept { return local(); }

    /**
     * Hand the bytes to the caller as a heap block, leaving this empty.
     *
     * Not a `std::string` member — the counterpart of `axl_string_steal()`,
     * for handing a buffer to C code that will `axl_free()` it. A SHORT
     * string has no heap block to give away, so one is allocated; the result
     * is `axl_free()`-able either way.
     *
     * On OOM (only reachable from the inline path, which has to allocate)
     * the string is left UNCHANGED and #axl::string::bad() is set -- the one
     * way a string that never exceeded #axl::string::sso_capacity can set it.
     *
     * @return the bytes to free with `axl_free()`, or NULL if empty or on OOM.
     */
    [[nodiscard]] char *steal() noexcept
    {
        if (m_size == 0) {
            clear();
            return nullptr;
        }
        char *out;
        if (local()) {
            out = (char *)axl_malloc(m_size + 1);
            if (out == nullptr) {
                m_bad = true;
                return nullptr;
            }
            axl_memcpy(out, m_ptr, m_size + 1);
        } else {
            out = m_ptr;
        }
        m_ptr  = m_sso;
        m_size = 0;
        m_sso[0] = '\0';
        return out;
    }

/** @cond INTERNAL
    Private storage and helpers. Hidden from the API reference because they
    are not API -- and because the anonymous union below renders as the
    malformed `union axl::string axl::string`, which Breathe cannot parse and
    which fails the zero-warning docs gate. */
private:
    /// Are the bytes inline? The pointer is self-referential exactly then.
    bool local() const noexcept { return m_ptr == m_sso; }

    /// Does @a p point into this string's own buffer?
    bool aliases_ptr(const char *p) const noexcept
    {
        return p != nullptr && p >= m_ptr && p <= m_ptr + m_size;
    }

    /// Take @a other's storage, leaving it empty. @a this must be empty.
    void adopt(string &other) noexcept
    {
        m_bad = other.m_bad;
        if (other.local()) {
            axl_memcpy(m_sso, other.m_sso, other.m_size + 1);
            m_ptr = m_sso;
        } else {
            m_ptr = other.m_ptr;
            m_cap = other.m_cap;
        }
        m_size = other.m_size;

        other.m_ptr    = other.m_sso;
        other.m_size   = 0;
        other.m_sso[0] = '\0';
        other.m_bad    = false;
    }

    /// Ensure capacity for @a n content bytes. Sets #bad() and fails softly.
    bool grow_to(size_type n)
    {
        if (n <= capacity()) {
            return true;
        }
        if (n > max_size()) {
            m_bad = true;
            return false;
        }

        /* Geometric, so repeated append stays amortised O(1) -- but clamped,
           because doubling a capacity near max_size() would wrap. */
        size_type want = capacity();
        want = (want > max_size() / 2) ? max_size() : want * 2;
        if (want < n) {
            want = n;
        }

        char *nb = (char *)axl_malloc(want + 1);
        if (nb == nullptr) {
            m_bad = true;
            return false;
        }
        axl_memcpy(nb, m_ptr, m_size + 1);   /* content and its terminator */

        if (!local()) {
            axl_free(m_ptr);
        }
        /* m_cap aliases m_sso, so this must come AFTER the copy out of it. */
        m_ptr = nb;
        m_cap = want;
        return true;
    }

    /// grow_to(m_size + n) with the addition checked rather than wrapped.
    bool grow_to_add(size_type n)
    {
        if (n > max_size() - m_size) {
            m_bad = true;
            return false;
        }
        return grow_to(m_size + n);
    }

    /// insert() with the source known not to overlap the buffer.
    string &insert_disjoint(size_type pos, const char *s, size_type n)
    {
        if (!grow_to_add(n)) {
            return *this;
        }
        axl_memmove(m_ptr + pos + n, m_ptr + pos, m_size - pos + 1);
        axl_memcpy(m_ptr + pos, s, n);
        m_size += n;
        return *this;
    }

    /// replace() with the source known not to overlap the buffer.
    string &replace_disjoint(size_type pos, size_type n,
                             const char *s, size_type slen)
    {
        /* Computed by hand here, so it is checked by hand here. insert() gets
           this free from grow_to_add(); a wrapped total would skip the
           reservation below, let erase() commit, and then be refused by
           insert_disjoint -- leaving the bytes destroyed and nothing put back,
           which is precisely what the reservation exists to prevent. */
        if (slen > max_size() - (m_size - n)) {
            m_bad = true;
            return *this;
        }
        size_type final_len = m_size - n + slen;

        if (final_len > m_size && !grow_to(final_len)) {
            return *this;    /* nothing touched yet -- still atomic */
        }
        erase(pos, n);
        return insert_disjoint(pos, s, slen);
    }

    /* 48 bytes: pointer, size, the inline buffer (which the heap capacity
       shares), and the sticky OOM flag. m_ptr is ALWAYS valid and points at
       m_sso exactly while the content is inline -- so reads never branch and
       there is no empty-string special case, which is how data() and c_str()
       can promise to be the same pointer. */
    char      *m_ptr;
    size_type  m_size;
    union {
        size_type m_cap;                    /* heap only: usable capacity */
        char      m_sso[sso_capacity + 1];  /* inline: content + NUL */
    };
    bool m_bad;
    /** @endcond */
};

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

inline bool
operator==(const string &a, const string &b) noexcept
{
    return a.view() == b.view();
}

inline bool
operator==(const string &a, std::string_view b) noexcept
{
    return a.view() == b;
}

inline bool
operator==(const string &a, const char *b) noexcept
{
    return a.view() == std::string_view(b != nullptr ? b : "");
}

inline std::strong_ordering
operator<=>(const string &a, const string &b) noexcept
{
    return a.view() <=> b.view();
}

inline std::strong_ordering
operator<=>(const string &a, std::string_view b) noexcept
{
    return a.view() <=> b;
}

inline std::strong_ordering
operator<=>(const string &a, const char *b) noexcept
{
    return a.view() <=> std::string_view(b != nullptr ? b : "");
}

/// Concatenation. Check #axl::string::bad() on the result.
inline string
operator+(const string &a, std::string_view b)
{
    string out;
    out.reserve(a.size() + b.size());
    out.append(a.data(), a.size());
    out.append(b.data(), b.size());
    return out;
}

/// @copydoc operator+(const string &, std::string_view)
inline string
operator+(const string &a, const char *b)
{
    return a + std::string_view(b != nullptr ? b : "");
}

/// @copydoc operator+(const string &, std::string_view)
inline string
operator+(std::string_view a, const string &b)
{
    string out;
    out.reserve(a.size() + b.size());
    out.append(a.data(), a.size());
    out.append(b.data(), b.size());
    return out;
}

/**
 * @copydoc operator+(const string &, std::string_view)
 *
 * Spelled out rather than left to the `string_view` overloads: with only
 * those two, `a + b` on two #axl::string needs one user-defined conversion on
 * whichever operand is not the view, so BOTH are viable and neither wins —
 * `a + b` was an ambiguity error. Nothing in the tree called it, so it
 * shipped invisible until a reviewer tried.
 */
inline string
operator+(const string &a, const string &b)
{
    return a + b.view();
}

/// @copydoc operator+(const string &, std::string_view)
inline string
operator+(const string &a, char c)
{
    return a + std::string_view(&c, 1);
}

/// @copydoc operator+(const string &, std::string_view)
inline string
operator+(char c, const string &b)
{
    return std::string_view(&c, 1) + b;
}

/**
 * @copydoc operator+(const string &, std::string_view)
 *
 * Spelled out for the same reason `operator+(const string &, const string &)`
 * is: without it `"literal" + s` is AMBIGUOUS between the two `string_view`
 * overloads and the string/string one, none of which wins. Same defect class,
 * found the same way -- by something finally calling it.
 */
inline string
operator+(const char *a, const string &b)
{
    return std::string_view(a != nullptr ? a : "") + b;
}

inline void
swap(string &a, string &b) noexcept
{
    a.swap(b);
}

} // namespace axl

#endif // AXL_STRING_HPP
