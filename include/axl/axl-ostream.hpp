/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ostream.hpp
 *
 * #axl::cout, #axl::cerr and #axl::endl — formatted output for FREESTANDING
 * C++, over `axl_printf` / `axl_printerr`.
 *
 * @par Why not std::cout
 *
 * Measured, not assumed. `std::cout << 42` needs 23 symbols from libstdc++,
 * none of them locale — but 11 of them are `_Unwind_*`, and shimming all 23
 * produced an image that LINKED (155 KB) and then took a `#PF` inside
 * `std::ostream::sentry::sentry` with `CR2 = -24`: `std::cout` had never been
 * constructed, because `ios_base::Init` is absent from the image entirely.
 *
 * Two things that look like fixes are not. Supplying our own
 * `std::streambuf` makes it WORSE (65 symbols, not 23) because the cost is
 * above the sink: `std::ostream`'s constructor calls `basic_ios::init()`,
 * which constructs a `std::locale`. And `<iostream>` collides with
 * `libaxl-cxx.a` — it drags in `functexcept.o`, `new_opv.o` and
 * `new_handler.o`, which multiply-define our `operator new[]`, the five
 * `std::__throw_*` stubs and `std::nothrow`, so linking needs
 * `--allow-multiple-definition`, which silences exactly the error class that
 * caught this SDK's `.init_array` and `.rela.dyn` bugs.
 *
 * So `std::` costs the unwinder plus `--allow-multiple-definition` and buys
 * only the spelling. This costs roughly 700 bytes over an equivalent
 * `axl_printf` program (x64 `--release`) and needs no `--hosted`. Treat that
 * as an order of magnitude, not a constant: `libaxl.a` is selectively
 * linked, so the figure is the difference between two DIFFERENT sets of
 * pulled objects and it drifts whenever the library does. Measured at 1227
 * and then 715 across one afternoon's changes to `axl-string.c` alone.
 * See `docs/AXL-Cxx-Stdlib-Surface.md` section 6.
 *
 * @par Why `cout` and not `out`
 *
 * `axl::err` is already the error constructor in axl-cxx.hpp
 * (`return axl::err(AXL_INVALID);`), so declaring `axl::err` as a stream
 * object is a hard compile error. Mirroring the standard's spelling sidesteps
 * that and keeps porting hosted code an `s/std::/axl::/`.
 *
 * @par Newlines
 *
 * #axl::endl writes `"\n"`, NOT `"\r\n"`. The console stream already
 * translates LF to CRLF on the way out (`console_transcode_crlf` in
 * `src/stream/axl-stream.c`), so emitting CRLF here would put `\r\r\n` on the
 * wire.
 *
 * @code
 * #include <axl/axl-ostream.hpp>
 *
 * int main(void)
 * {
 *     axl::cout << "hello " << 42 << ' ' << 3.5 << ' ' << true << axl::endl;
 *     axl::cerr << "to stderr" << axl::endl;
 *     return 0;
 * }
 * @endcode
 */

#ifndef AXL_OSTREAM_HPP
#define AXL_OSTREAM_HPP

#ifndef __cplusplus
#error "axl-ostream.hpp is C++ only"
#endif

#include <stdint.h>

#include <string_view>

#include <axl/axl-cxx.hpp>
#include <axl/axl-stream.h>
#include <axl/axl-string.hpp>

namespace axl {

/**
 * A formatted output sink over one of AXL's standard streams.
 *
 * Not constructible by consumers beyond the two objects below — there is no
 * file or string variant yet, and `axl_fprintf` on an #AxlStream is the way
 * to write anywhere else.
 *
 * Every `operator<<` calls `axl_printf` with a LITERAL format string, one
 * overload per type. A variadic forwarder taking a runtime format would be
 * shorter and would defeat `-Wformat` and clang-tidy entirely.
 */
class ostream {
public:
    /**
     * @param to_err  write to standard error rather than standard output.
     *
     * `constexpr` on purpose: it makes #axl::cout and #axl::cerr
     * constant-initialised, so they land in `.data` with NO `.init_array`
     * entry. That removes any static-initialisation-order question, and it
     * is why the prototype worked even while `--gc-sections` was still
     * eating `.init_array` (fixed in 8db522c1).
     */
    constexpr explicit ostream(bool to_err) noexcept : m_err(to_err) {}

    ostream(const ostream &)            = delete;
    ostream &operator=(const ostream &) = delete;

    ostream &operator<<(const char *s)
    {
        return put(s != nullptr ? std::string_view(s) : std::string_view("(null)"));
    }

    ostream &operator<<(std::string_view v) { return put(v); }
    ostream &operator<<(const string &s)    { return put(s.view()); }

    ostream &operator<<(char c)
    {
        return put(std::string_view(&c, 1));
    }

    ostream &operator<<(bool v)
    {
        return put(v ? std::string_view("true") : std::string_view("false"));
    }

    /**
     * Writes `"nullptr"`.
     *
     * `std::ostream` has carried this overload since C++17 for the same
     * reason: without it `stream << nullptr` is AMBIGUOUS, because a null
     * pointer constant converts equally well to `const char *`, `const void *`
     * and `bool`.
     */
    ostream &operator<<(decltype(nullptr))
    {
        return put(std::string_view("nullptr"));
    }

    ostream &operator<<(int v)
    {
        if (m_hex) { emit("%x", (unsigned int)v); } else { emit("%d", v); }
        return *this;
    }
    ostream &operator<<(unsigned int v)
    {
        emit(m_hex ? "%x" : "%u", v);
        return *this;
    }
    ostream &operator<<(long v)
    {
        if (m_hex) { emit("%lx", (unsigned long)v); } else { emit("%ld", v); }
        return *this;
    }
    ostream &operator<<(unsigned long v)
    {
        emit(m_hex ? "%lx" : "%lu", v);
        return *this;
    }
    ostream &operator<<(long long v)
    {
        if (m_hex) { emit("%llx", (unsigned long long)v); } else { emit("%lld", v); }
        return *this;
    }
    ostream &operator<<(unsigned long long v)
    {
        emit(m_hex ? "%llx" : "%llu", v);
        return *this;
    }

    /**
     * Written with `%g` — the shortest of fixed and exponential form, as
     * `printf("%g")`. AXL's own float formatting is what backs it, so the
     * output round-trips through `axl_str_to_double()`.
     */
    ostream &operator<<(double v)             { emit("%g", v);   return *this; }
    ostream &operator<<(float v)              { emit("%g", (double)v); return *this; }

    ostream &operator<<(const void *p)        { emit("%p", p);   return *this; }

    /// Applies a manipulator, so `s << axl::endl` works.
    ostream &operator<<(ostream &(*manip)(ostream &)) { return manip(*this); }

    /// Push any buffered bytes to the device.
    ostream &flush()
    {
        axl_fflush(stream());
        return *this;
    }

    /// Write integers in base 16 or base 10. See #axl::hex and #axl::dec.
    void base(int b) noexcept { m_hex = (b == 16); }

    /// The base integers are currently written in.
    int base() const noexcept { return m_hex ? 16 : 10; }

private:
    AxlStream *stream() const { return m_err ? axl_stderr : axl_stdout; }

    ostream &put(std::string_view v)
    {
        if (!v.empty()) {
            axl_fwrite(v.data(), 1, v.size(), stream());
        }
        return *this;
    }

    /* One literal format per overload, so -Wformat still checks the call. */
    void emit(const char *fmt, int v)                { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, unsigned int v)       { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, long v)               { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, unsigned long v)      { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, long long v)          { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, unsigned long long v) { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, double v)             { axl_fprintf(stream(), fmt, v); }
    void emit(const char *fmt, const void *v)        { axl_fprintf(stream(), fmt, v); }

    bool m_err;
    bool m_hex = false;
};

/**
 * Write a newline.
 *
 * Writes `"\n"`, not `"\r\n"` — the console stream translates LF to CRLF
 * itself, so a CRLF here would reach the wire as `\r\r\n`.
 *
 * Unlike `std::endl` this does NOT flush: AXL's standard streams default to
 * #AXL_STREAM_BUF_NONE, so there is normally nothing buffered to push. Call
 * #axl::ostream::flush() explicitly on a stream you have buffered yourself.
 */
inline ostream &
endl(
    ostream &o    ///< the stream to write to
)
{
    return o << '\n';
}

/// Flush @a o, as `std::flush`.
inline ostream &
flush(
    ostream &o    ///< the stream to flush
)
{
    return o.flush();
}

/**
 * Write integers in hexadecimal, as `std::hex`.
 *
 * There is an #axl::istream overload of this name too, so `axl::hex` works on
 * either side. That is not decoration: with an input-only manipulator,
 * `axl::cout << axl::hex` still COMPILED — the function-to-pointer conversion
 * made `operator<<(bool)` the only viable candidate, and it printed `true`.
 */
inline ostream &
hex(
    ostream &o    ///< the stream to reconfigure
)
{
    o.base(16);
    return o;
}

/// Write integers in decimal, as `std::dec`. The default.
inline ostream &
dec(
    ostream &o    ///< the stream to reconfigure
)
{
    o.base(10);
    return o;
}

/// Standard output. Constant-initialised; see #axl::ostream::ostream().
inline ostream cout{false};

/// Standard error. Constant-initialised; see #axl::ostream::ostream().
inline ostream cerr{true};

} // namespace axl

#endif // AXL_OSTREAM_HPP
