/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-string-inst.cpp
    An explicit instantiation of the one `std::string` member that is
    emitted out of line and lives in libstdc++'s `string-inst.o`.

    `basic_string<char>::_M_replace_cold` backs `replace()`,
    `insert(pos, const char *)` and some `map<K, string>` literal
    assignments.  Whether the compiler emits a call to it is an INLINING
    decision, so without this the link fails or succeeds depending on
    `-O` level and surrounding code shape -- the worst kind of failure to
    diagnose.

    @par Why not just link string-inst.o

    Because pulling that member cascades: it was compiled WITH exceptions,
    so it drags `eh_personality.o` -> `eh_throw.o` ->
    `_Unwind_RaiseException`, `fputs`, `fprintf` -- the unwinder this SDK
    exists without.  Instantiating the template ourselves under
    `-fno-exceptions` produces an object whose ONLY undefined symbol is
    `memmove`, which `libaxl.a` already supplies.

    This is not a reimplementation and claims no cleverness: it asks the
    compiler to emit libstdc++'s own template body with our flags.  That
    is an ordinary use of the headers, and the object is compiler output
    -- Target Code under the GCC Runtime Library Exception, exactly like
    every other object built from `<string>`.
**/

#include <string>

namespace std {
_GLIBCXX_BEGIN_NAMESPACE_VERSION
_GLIBCXX_BEGIN_NAMESPACE_CXX11

template void
basic_string<char>::_M_replace_cold(char *, size_type, const char *,
                                    size_type, size_type);

_GLIBCXX_END_NAMESPACE_CXX11
_GLIBCXX_END_NAMESPACE_VERSION
} // namespace std
