/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-terminate.cpp
    What an uncaught exception says, and the 112 KB it stops costing.

    libstdc++ installs `__gnu_cxx::__verbose_terminate_handler` as the default
    terminate handler -- `__cxxabiv1::__terminate_handler` is initialised to it
    in `eh_term_handler.o` -- so every `-fexceptions` image links
    `vterminate.o`, and that object pulls `__cxa_demangle` and newlib's stdio
    behind it.

    MEASURED with and without this object, on `cxx-exceptions-selftest.cpp`
    (`--release -fexceptions`, both arches):

        x64    264,185 -> 150,920    -113,265  (-42.9%)
        aa64   259,933 -> 139,651    -120,282  (-46.3%)

    AND THE STOCK HANDLER PRINTS NOTHING UNDER UEFI. Verified by booting an
    uncaught throw: it writes to a newlib `stderr` no UEFI image wires up, so
    the 112 KB buys negative value -- an image that is bigger AND silent. Ours
    is smaller and is the only one that speaks.

    THE MECHANISM IS PREEMPTION, and it needs this to be an OBJECT on the link
    line rather than an archive member. An archive member is pulled only for a
    symbol still undefined, and defining the symbol here first means
    `vterminate.o` is never pulled at all -- so its `__cxa_demangle` reference
    never arrives to be satisfied. From an archive the link would resolve
    either way and the demangler could still come in. This is the same
    reasoning axl-cxxrt-alloc.c documents for `malloc`; see `axl-cc` for where
    the three objects are named.

    That preemption is safe because `vterminate.o` defines nothing else: its
    only other symbols are the handler's `.cold` half and its function-local
    static. Were it pulled for some third symbol, this would be a duplicate
    definition instead of a replacement.

    KEEPING THE EXCEPTION'S IDENTITY IS NEARLY FREE, which is why this does
    more than print a fixed string. Measured during the spike against a bare
    "terminate called" variant, the type name plus `what()` cost +89 bytes on
    x64 and +655 on aa64 -- so the tradeoff the design expected here (lose the
    type name, or keep the demangler) turned out not to exist:
    `abi::__cxa_current_exception_type()` hands back the type_info the
    ABI already had to store, and rethrowing recovers `what()` from a real
    catch. The name is MANGLED, and that is the trade taken deliberately --
    demangling `St13runtime_error` into `std::runtime_error` is the whole
    112 KB.

    Compiled with `-fexceptions` where the rest of AXL's C++ is not: the
    `throw;` below is not decoration, it is the only way to reach `what()` on
    an exception whose static type is unknown. That is also why this file is
    `.cpp` and its three siblings are `.c`.
**/

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

#include <axl.h>

/**
 * @brief Report an uncaught exception and exit, replacing libstdc++'s.
 *
 * Defined by its qualified name rather than inside a reopened `namespace
 * __gnu_cxx`: the declaration in `<exception>` is the one that must be
 * matched, and a qualified definition cannot silently declare a NEW function
 * in that namespace if the spelling ever drifts.
 *
 * Not marked `AXL_NORETURN` even though it never returns. `<exception>`
 * declares it without the attribute, and adding one on a later declaration is
 * ill-formed. libsupc++'s `__terminate` calls `abort()` after the handler
 * anyway, so a return would still be fatal; it just would not be ours.
 */
void
__gnu_cxx::__verbose_terminate_handler(
    void
    )
{
    /* Re-entry is reachable, not theoretical: `what()` on a broken exception
       object can throw, and that throw terminates. Exit on the second pass
       rather than recursing until the stack runs out -- the first pass has
       already printed the type name, which is the diagnostically useful half.
       Not `axl_atomic`: UEFI boot services are single-threaded here, and this
       runs on the throwing processor. */
    static bool sTerminating;

    if (sTerminating) {
        axl_exit(1);
    }
    sTerminating = true;

    /* NULL when nothing is in flight -- a bare std::terminate() call, or a
       rethrow with no active exception. libstdc++'s handler distinguishes the
       two cases and so does this: "no exception" is a different bug from "an
       exception nobody caught", and printing a type name for the first would
       be a lie. */
    const std::type_info *type = abi::__cxa_current_exception_type();

    if (type == nullptr) {
        axl_printf("terminate: called with no exception in flight\n");
        axl_exit(1);
    }

    axl_printf("terminate: uncaught exception of type %s\n", type->name());

    /* The rethrow is how the message gets a `what()`. There is no ABI call
       that yields it -- `what()` is virtual on `std::exception`, so reaching
       it needs a reference of that static type, which only a handler can
       produce. Rethrowing the in-flight exception inside our own try block is
       what libstdc++'s handler does, for the same reason. */
    try {
        throw;
    } catch (const std::exception &e) {
        axl_printf("  what(): %s\n", e.what());
    } catch (...) {
        /* Not derived from std::exception -- `throw 42;`, or a user type that
           does not inherit it. SAYING SO beats staying silent: the reader has
           just been given a mangled type name and would otherwise have to work
           out whether what() was empty or absent.

           Catching at all is required either way. Letting this escape would
           leave the handler through libsupc++'s `__catch(...) { abort(); }`,
           which loses the exit path _exit() routes to axl_exit for. */
        axl_printf("  no what(): not derived from std::exception\n");
    }

    axl_exit(1);
}
