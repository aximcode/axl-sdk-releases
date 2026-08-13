/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-istream.hpp
 *
 * #axl::cin — formatted input for FREESTANDING C++, over `axl_readline`.
 *
 * See axl-ostream.hpp for why this layer exists rather than `std::cin` (the
 * short version: `std::cin` needs the same 23 libstdc++ symbols `std::cout`
 * does, 11 of them `_Unwind_*`, and the resulting image faults because
 * `ios_base::Init` never ran).
 *
 * @par Two ways to handle failure, one state
 *
 * `<<` can just return the stream; `>>` has to report that the text did not
 * parse. Both spellings below observe the SAME sticky state, so they mix
 * freely:
 *
 * @code
 * axl::cin >> port >> host;        // chains
 * if (!axl::cin) {                 // one check covers the run
 *     return axl::err(AXL_INVALID);
 * }
 *
 * auto p = axl::cin.read<uint16_t>();   // axl::result<uint16_t>
 * if (!p) {
 *     return axl::err(p.error());
 * }
 * @endcode
 *
 * #axl::istream::read() sets the sticky state too, and that is load-bearing rather than
 * incidental: `axl_str_to_u64()` REWINDS its `endptr` to the start on
 * overflow (documented, and deliberately unlike `axl_str_to_double()`), so a
 * cursor driven off `endptr` makes no progress on input like
 * `"99999999999999999999"`. Without a sticky bit,
 * `while (auto v = cin.read<int>())` would spin on that token forever.
 * #axl::istream::clear() resets the state when you want to try again.
 *
 * @par Extraction semantics are libstdc++'s
 *
 * `>>` skips leading whitespace, extracts up to the next whitespace or
 * end-of-input, and sets the fail state if it extracts nothing. Newline is
 * ordinary whitespace, so `cin >> a >> b` spans lines. On a parse failure the
 * cursor is left AT the offending character and the target is not modified,
 * which is where `std::` leaves it too.
 *
 * One divergence, deliberate: `std::` stores a saturated value on an
 * out-of-range number (C++11 `num_get`). This leaves the target UNCHANGED,
 * because AXL's integer parsers report syntax errors and range errors
 * identically — a saturated value here would be indistinguishable from a real
 * one, and silently wrong is worse than untouched.
 *
 * @par Where the bytes come from
 *
 * `axl_stdin_text()`, created on first extraction and released through
 * `axl_atexit`. That decodes the UEFI Shell's default `|` pipe, which
 * transcodes to UCS-2 — an encoding the writer never chose and one POSIX
 * `stdin` never inserts. `<` redirection and interactive input pass through
 * unchanged. Reading raw `axl_stdin` instead would make `echo 42 | prog`
 * parse as `4`.
 *
 * Input is read a LINE at a time. That is what makes `<` redirection, `|`
 * pipes and an interactive console all work through one path — the console
 * line editor only exists at the `axl_readline` level. Reading keystrokes
 * instead (`axl_console_read_key`) cannot see a redirect at all.
 */

#ifndef AXL_ISTREAM_HPP
#define AXL_ISTREAM_HPP

#ifndef __cplusplus
#error "axl-istream.hpp is C++ only"
#endif

#include <stdint.h>

#include <concepts>
#include <string_view>
#include <type_traits>

#include <axl/axl-atexit.h>
#include <axl/axl-cxx.hpp>
#include <axl/axl-mem.h>
#include <axl/axl-stream.h>
#include <axl/axl-str.h>
#include <axl/axl-string.hpp>

namespace axl {

/**
 * Any type `>>` can fill with bytes: anything offering `assign(ptr, len)` and
 * `clear()`.
 *
 * Spelled as a requirement rather than a list of overloads so the SAME
 * `operator>>` serves #axl::string freestanding AND `std::string` under
 * `axl-c++ --hosted`, without this header including `<string>` (which is
 * hosted-only and would make the header unusable freestanding).
 */
template <class S>
concept byte_sink = requires(S &s, const char *p, size_t n) {
    s.assign(p, n);
    s.clear();
};

namespace detail {

/**
 * Construction key for #axl::cin.
 *
 * Consumers have no reason to name this. An #axl::istream registers an
 * `axl_atexit` handler against its OWN address on first use, and it has no
 * destructor to withdraw that registration — one would force a
 * `__cxa_atexit` call at static-init time and put back the `.init_array`
 * entry the design exists to avoid. So a local `axl::istream` would leave the
 * handler pointing at dead stack, to be written through at exit.
 */
struct cin_key {
    explicit cin_key() = default;
};

} // namespace detail

/**
 * A formatted input source over the UEFI shell's standard input.
 *
 * #axl::cin is the instance to use. See the file documentation for the
 * failure model, the extraction rules, and why input is line-oriented.
 */
class istream {
public:
    /**
     * `constexpr` on purpose: it makes #axl::cin constant-initialised, so it
     * lands in `.data` with NO `.init_array` entry and no
     * static-initialisation-order question. The same reason
     * #axl::ostream::ostream() is `constexpr`, and the reason this class has
     * no destructor — one would force a `__cxa_atexit` registration at static
     * init time, putting the entry back. The line buffer and the decoding
     * source are released through `axl_atexit` instead.
     */
    constexpr explicit istream(detail::cin_key) noexcept {}

    /**
     * Deleted: #axl::cin is the only instance.
     *
     * See #axl::detail::cin_key — a second instance would arm an `axl_atexit`
     * handler against its own address with no destructor to withdraw it.
     */
    istream()                           = delete;
    istream(const istream &)            = delete;
    istream &operator=(const istream &) = delete;

    // -----------------------------------------------------------------
    // State
    // -----------------------------------------------------------------

    /// True while no extraction has failed. `if (!axl::cin)` is the idiom.
    explicit operator bool() const noexcept { return !m_fail; }

    /// Did an extraction fail?
    bool fail() const noexcept { return m_fail; }

    /// Is the input exhausted?
    bool eof() const noexcept { return m_eof; }

    /// Why the last extraction failed, or #AXL_OK if none has.
    AxlStatus status() const noexcept { return m_status; }

    /**
     * Forget a previous failure so extraction can be attempted again.
     *
     * Clears the end-of-input flag too, as `std::ios::clear()` clears
     * `eofbit`. If the input really is exhausted the next extraction sets it
     * straight back.
     */
    void clear() noexcept
    {
        m_fail   = false;
        m_eof    = false;
        m_status = AXL_OK;
    }

    // -----------------------------------------------------------------
    // Extraction
    // -----------------------------------------------------------------

    /**
     * Extract a single non-whitespace character, as `std::istream`'s.
     *
     * @note `uint8_t` and `int8_t` extract a NUMBER here, where
     *     `std::istream` extracts a character — they are `unsigned char` and
     *     `signed char`, which the standard treats as character types. Under
     *     UEFI a byte-sized field in config text is far more often a number,
     *     so this reads `"200"` as 200 rather than as `'2'`. Spell the type
     *     `char` when you want a character.
     */
    istream &operator>>(char &v)
    {
        if (m_fail) {
            return *this;
        }
        if (!skip_ws()) {
            set_fail(AXL_NOT_FOUND);
            return *this;
        }
        v = m_line[m_pos];
        m_pos++;
        return *this;
    }

    istream &operator>>(uint8_t &v)  { return num(v, axl_str_to_u8); }
    istream &operator>>(uint16_t &v) { return num(v, axl_str_to_u16); }
    istream &operator>>(uint32_t &v) { return num(v, axl_str_to_u32); }
    istream &operator>>(uint64_t &v) { return num(v, axl_str_to_u64); }
    istream &operator>>(int8_t &v)   { return num(v, axl_str_to_s8); }
    istream &operator>>(int16_t &v)  { return num(v, axl_str_to_s16); }
    istream &operator>>(int32_t &v)  { return num(v, axl_str_to_s32); }
    istream &operator>>(int64_t &v)  { return num(v, axl_str_to_s64); }

    istream &operator>>(double &v)
    {
        return real(v, axl_str_to_double);
    }

    istream &operator>>(float &v)
    {
        return real(v, axl_str_to_float);
    }

    /**
     * Extract a whitespace-delimited token.
     *
     * `true`/`false` and `1`/`0` are all accepted, matching `std::boolalpha`
     * and the default numeric form at once — under UEFI both spellings turn
     * up in config text and refusing one buys nothing.
     */
    istream &operator>>(bool &v)
    {
        std::string_view tok;
        if (!token(tok)) {
            return *this;
        }
        if (tok == "true" || tok == "1") {
            v = true;
        } else if (tok == "false" || tok == "0") {
            v = false;
        } else {
            set_fail(AXL_INVALID);
            return *this;
        }
        consume(tok.size());
        return *this;
    }

    /// Extract a whitespace-delimited token into any #axl::byte_sink.
    template <byte_sink S>
    istream &operator>>(S &out)
    {
        std::string_view tok;
        if (!token(tok)) {
            return *this;
        }
        out.assign(tok.data(), tok.size());
        consume(tok.size());

        /* A sink that reports allocation failure must be BELIEVED. Without
           this, `axl::cin >> s` under exhaustion returned success while `s`
           silently kept its previous token -- the stream said it had read
           something it had not. Guarded on the member existing, because
           #axl::byte_sink deliberately admits std::string, which has none. */
        if constexpr (requires { { out.bad() } -> std::convertible_to<bool>; }) {
            if (out.bad()) {
                set_fail(AXL_NO_RESOURCES);
            }
        }
        return *this;
    }

    /**
     * Extract a whitespace-delimited token into a fixed buffer.
     *
     * The capacity comes from the array type, so there is no length argument
     * to get wrong — this is the overload `std::istream`'s
     * `operator>>(char *)` should always have been. A token longer than
     * `N - 1` bytes is truncated, consumed, and sets the fail state.
     */
    template <size_t N>
    istream &operator>>(char (&out)[N])
    {
        static_assert(N > 1, "axl::cin >> buf needs room for a byte and a NUL");

        std::string_view tok;
        if (!token(tok)) {
            return *this;
        }

        size_t n = tok.size() < N - 1 ? tok.size() : N - 1;
        axl_memcpy(out, tok.data(), n);
        out[n] = '\0';
        consume(tok.size());

        if (tok.size() > N - 1) {
            set_fail(AXL_NO_RESOURCES);
        }
        return *this;
    }

    /// Applies a manipulator, so `axl::cin >> axl::hex` works.
    istream &operator>>(istream &(*manip)(istream &)) { return manip(*this); }

    /**
     * Extract one value and return it, rather than reporting through the
     * stream state.
     *
     * @return the value, or the #AxlStatus that says why not: #AXL_NOT_FOUND
     *     when the input is exhausted, #AXL_INVALID when the text did not
     *     parse, #AXL_NO_RESOURCES on a truncated token.
     */
    template <class T>
    [[nodiscard]] result<T> read()
    {
        T tmp{};
        *this >> tmp;
        if (m_fail) {
            return err(m_status);
        }
        return tmp;
    }

    /**
     * Read up to @a delim into @a out, which is cleared first.
     *
     * The delimiter is consumed but not stored, as `std::getline`'s. Sets the
     * fail state only when nothing at all was read because the input was
     * already exhausted — a final line with no trailing delimiter succeeds.
     */
    template <byte_sink S>
    istream &getline(S &out, char delim = '\n')
    {
        if (m_fail) {
            return *this;
        }
        out.clear();

        /* Accumulate locally and assign ONCE. Appending straight into @a out
           would need data()/size() from it, which #axl::byte_sink does not
           promise, and would re-copy the whole line on every refill. */
        string acc;
        bool   any = false;

        for (;;) {
            if (!has_input()) {
                if (!any) {
                    set_fail(m_eof ? AXL_NOT_FOUND : AXL_IO_ERROR);
                } else {
                    finish(out, acc);
                }
                return *this;
            }

            const char *from = m_line + m_pos;
            size_t      left = m_len - m_pos;
            size_t      i    = 0;

            while (i < left && from[i] != delim) {
                i++;
            }

            if (i != 0) {
                acc.append(from, i);
                any = true;
            }
            m_pos += i;

            if (i < left) {          /* found the delimiter */
                m_pos++;             /* consume it, do not store it */
                finish(out, acc);
                return *this;
            }
        }
    }

    /**
     * Set the radix `>>` reads integers in. See #axl::hex and #axl::dec.
     *
     * Any base `axl_str_to_u64()` accepts works, so `base(8)` reads octal —
     * there is no `axl::oct` manipulator, for the reason given beside
     * #axl::hex.
     */
    void base(int b) noexcept { m_base = b; }

    /// The radix `>>` currently reads integers in.
    int base() const noexcept { return m_base; }

private:
    /* ---- source ------------------------------------------------------- */

    /* noexcept because AXL invokes it from its own C teardown frames:
       an exception escaping here would unwind through code that carries
       no landing pads, running none of its cleanup. See
       #AXL_CB_NOEXCEPT. */
    static void release(void *self) noexcept
    {
        istream *s = static_cast<istream *>(self);
        axl_free(s->m_line);
        s->m_line = nullptr;
        s->m_len  = 0;
        s->m_pos  = 0;
        if (s->m_src != nullptr) {
            axl_fclose(s->m_src);
            s->m_src = nullptr;
        }
    }

    AxlStream *source()
    {
        if (m_src == nullptr && !m_src_tried) {
            m_src_tried = true;
            /* The decoding wrapper, not raw axl_stdin: the shell's default
               `|` transcodes to UCS-2 and raw bytes would parse as garbage.
               Registered rather than destructed -- see the constructor. */
            m_src = axl_stdin_text();
            if (m_src != nullptr && axl_atexit(release, this) == 0) {
                /* Nothing else frees this stream or the line buffer, so a
                   registration we could not make means we cannot own them.
                   Give the stream back rather than leak it past the gate. */
                axl_fclose(m_src);
                m_src = nullptr;
            }
        }
        return m_src;
    }

    /// Pull the next line. False at end of input.
    bool refill()
    {
        AxlStream *s = source();
        if (s == nullptr) {
            m_eof = true;
            return false;
        }

        axl_free(m_line);
        m_line = axl_readline(s);
        m_pos  = 0;
        m_len  = m_line != nullptr ? axl_strlen(m_line) : 0;

        /* A zero-length line would make has_input() spin: it loops while
           `m_pos >= m_len`, and a refill that changes neither ends nothing.
           axl_readline keeps the delimiter, so today a non-NULL line is
           always at least one byte -- this does not depend on that holding
           in another file forever. */
        if (m_line == nullptr || m_len == 0) {
            m_eof = true;
            return false;
        }
        return true;
    }

    /// True when at least one unread byte is buffered.
    bool has_input()
    {
        while (m_line == nullptr || m_pos >= m_len) {
            if (!refill()) {
                return false;
            }
        }
        return true;
    }

    /* ---- tokenising --------------------------------------------------- */

    static bool is_space(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\v' || c == '\f';
    }

    /// Advance past whitespace, crossing lines. False at end of input.
    bool skip_ws()
    {
        for (;;) {
            if (!has_input()) {
                return false;
            }
            while (m_pos < m_len && is_space(m_line[m_pos])) {
                m_pos++;
            }
            if (m_pos < m_len) {
                return true;
            }
        }
    }

    /**
     * The next whitespace-delimited token, WITHOUT consuming it.
     *
     * Left unconsumed so a failed conversion leaves the cursor at the
     * offending character, which is where `std::` leaves it.
     */
    bool token(std::string_view &out)
    {
        if (m_fail) {
            return false;
        }
        if (!skip_ws()) {
            set_fail(AXL_NOT_FOUND);
            return false;
        }

        size_t end = m_pos;
        while (end < m_len && !is_space(m_line[end])) {
            end++;
        }
        out = std::string_view(m_line + m_pos, end - m_pos);
        return true;
    }

    void consume(size_t n) { m_pos += n; }

    void set_fail(AxlStatus s) noexcept
    {
        m_fail   = true;
        m_status = s;
    }

    /// Hand the accumulated line over, reporting an accumulation OOM.
    template <byte_sink S>
    void finish(S &out, const string &acc)
    {
        if (acc.bad()) {
            set_fail(AXL_NO_RESOURCES);
            return;
        }
        out.assign(acc.data(), acc.size());
        if constexpr (requires { { out.bad() } -> std::convertible_to<bool>; }) {
            if (out.bad()) {
                set_fail(AXL_NO_RESOURCES);
            }
        }
    }

    /* ---- numeric extraction ------------------------------------------- */

    template <class T, class Fn>
    istream &num(T &v, Fn parse)
    {
        std::string_view tok;
        if (!token(tok)) {
            return *this;
        }

        const char *nptr = tok.data();
        const char *end  = nullptr;
        T           tmp{};

        if (parse(nptr, m_base, &tmp, &end) != AXL_OK || end == nptr) {
            /* Cursor stays at the offending character, target untouched. */
            set_fail(AXL_INVALID);
            return *this;
        }

        consume(static_cast<size_t>(end - nptr));
        v = tmp;
        return *this;
    }

    template <class T, class Fn>
    istream &real(T &v, Fn parse)
    {
        std::string_view tok;
        if (!token(tok)) {
            return *this;
        }

        const char *nptr = tok.data();
        const char *end  = nullptr;
        T           tmp{};

        if (parse(nptr, &tmp, &end) != AXL_OK || end == nptr) {
            set_fail(AXL_INVALID);
            return *this;
        }

        consume(static_cast<size_t>(end - nptr));
        v = tmp;
        return *this;
    }

    AxlStream *m_src       = nullptr;
    char      *m_line      = nullptr;
    size_t     m_len       = 0;
    size_t     m_pos       = 0;
    int        m_base      = 10;
    AxlStatus  m_status    = AXL_OK;
    bool       m_fail      = false;
    bool       m_eof       = false;
    bool       m_src_tried = false;
};

/// Read integers as hexadecimal, as `std::hex`.
inline istream &
hex(
    istream &s    ///< the stream to reconfigure
)
{
    s.base(16);
    return s;
}

/// Read integers as decimal, as `std::dec`. The default.
inline istream &
dec(
    istream &s    ///< the stream to reconfigure
)
{
    s.base(10);
    return s;
}

/* There is deliberately no `axl::oct`. A manipulator that exists for only ONE
   stream direction is a trap rather than a feature: `axl::cout << axl::oct`
   would still COMPILE, because the function-to-pointer conversion makes
   operator<<(bool) viable, and it would print `true`. #axl::hex and #axl::dec
   have overloads on both sides, so they resolve exactly. AXL's format engine
   has no `%o`, so an output twin for octal cannot be written -- and octal
   INPUT is still available as `axl::cin.base(8)`. */

/**
 * Read up to @a delim from @a in into @a out.
 *
 * The free-function spelling of #axl::istream::getline, so
 * `axl::getline(axl::cin, line)` reads as `std::getline` does.
 */
template <byte_sink S>
inline istream &
getline(
    istream &in,           ///< the stream to read from
    S       &out,          ///< cleared, then filled with the line
    char     delim = '\n'  ///< the terminator; consumed, never stored
)
{
    return in.getline(out, delim);
}

/// Standard input. Constant-initialised; see #axl::istream::istream().
inline istream cin{detail::cin_key{}};

} // namespace axl

#endif // AXL_ISTREAM_HPP
