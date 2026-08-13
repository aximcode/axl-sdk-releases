/**
 * cxx-errors.cpp — the C++ layer's error vocabulary and RAII, in use.
 *
 * Build with: axl-c++ cxx-errors.cpp -o cxx-errors.efi
 *
 * Freestanding: this needs no --hosted. `axl::result` is
 * `std::expected`, which is in the C++23 freestanding subset, so the
 * error model works in the smallest possible image.
 *
 * Two things every C++ consumer of AXL has to know, and nothing else
 * in the examples showed either:
 *
 *   1. A fallible function returns axl::result<T> — the value, or the
 *      AxlStatus saying why not. The same status codes a C caller
 *      branches on, so the two sides need no translation table.
 *
 *   2. `.value()` on an errored result HALTS. Under -fno-exceptions
 *      libstdc++ lowers every throw site to abort(), and the SDK
 *      defines abort() so it is a diagnosable stop rather than a link
 *      error. On any path that can actually fail you want has_value(),
 *      value_or() or operator* after a check — which is what this
 *      example does, deliberately, on every access.
 *
 * The C library's AXL_AUTOPTR works from C++ too, so a C handle gets a
 * destructor without anyone writing one.
 */

#include <axl.h>
#include <axl/axl-cxx.hpp>

namespace {

/* A fallible parse, in the shape the layer intends: the value on
 * success, a specific AxlStatus on failure. AXL_INVALID rather than
 * the generic AXL_ERR, because the caller can meaningfully tell this
 * apart from "no such thing". */
axl::result<uint16_t>
parse_port(
    const char *text
)
{
    if (text == nullptr || *text == '\0') {
        return axl::err(AXL_INVALID);
    }

    uint32_t value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return axl::err(AXL_INVALID);
        }
        value = value * 10 + (uint32_t) (*p - '0');
        if (value > 65535) {
            return axl::err(AXL_INVALID);
        }
    }
    if (value == 0) {
        return axl::err(AXL_INVALID);        /* port 0 is not bindable */
    }
    return (uint16_t) value;
}

/* Errors compose: a caller can propagate without unwrapping, because
 * the error type is the same all the way down. */
axl::result<uint32_t>
parse_pair(
    const char *a,   ///< first port
    const char *b    ///< second port
)
{
    auto first = parse_port(a);
    if (!first.has_value()) {
        return axl::err(first.error());
    }
    auto second = parse_port(b);
    if (!second.has_value()) {
        return axl::err(second.error());
    }
    return ((uint32_t) *first << 16) | *second;
}

void
report(
    const char           *label,   ///< what was parsed
    axl::result<uint16_t> r        ///< its outcome
)
{
    if (r.has_value()) {
        axl_printf("  %-10s ok    port=%u\r\n", label, (unsigned) *r);
    } else {
        /* Never r.value() here — that is the halt. */
        axl_printf("  %-10s error status=%d fallback=%u\r\n",
                   label, (int) r.error(), (unsigned) r.value_or(8080));
    }
}

} // namespace

int
main(void)
{
    axl_print("cxx-errors: parsing\r\n");
    report("\"443\"",  parse_port("443"));
    report("\"\"",     parse_port(""));
    report("\"70000\"", parse_port("70000"));
    report("\"8o8\"",  parse_port("8o8"));

    auto pair = parse_pair("80", "443");
    axl_printf("cxx-errors: pair ok=%d packed=0x%08x\r\n",
               pair.has_value(), (unsigned) pair.value_or(0));

    auto bad = parse_pair("80", "nope");
    axl_printf("cxx-errors: pair ok=%d status=%d\r\n",
               bad.has_value(), (int) bad.error());

    /* RAII over a C handle: AXL_AUTOPTR works from C++, so the arena
     * is released on every exit path from this scope, including an
     * early return added later by someone who never reads this
     * comment. That scope-guard mechanism is the one thing AXL's only
     * C++ consumer had already hand-rolled before the layer existed. */
    {
        AXL_AUTOPTR(AxlArena) arena = axl_arena_new(4096);
        if (arena == nullptr) {
            axl_print("cxx-errors: arena unavailable\r\n");
            return 1;
        }
        void *block = axl_arena_alloc(arena, 256);
        axl_printf("cxx-errors: arena block=%d remaining=%zu\r\n",
                   block != nullptr, axl_arena_remaining(arena));
    }
    axl_print("cxx-errors: arena released by scope exit\r\n");

    axl_print("cxx-errors: done\r\n");
    return 0;
}
