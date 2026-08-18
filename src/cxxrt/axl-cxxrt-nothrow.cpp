/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt-nothrow.cpp
    A `__cxa_throw` that DIAGNOSES and exits without entering the unwinder.

    Linked only by `axl-c++ --no-eh-frame`, in place of axl-cxxrt-eh.o. That
    build drops the `.eh_frame` KEEP -- about 18% of a C++ image -- and with it
    the frame table the unwinder needs.

    @par What this replaces, and why the naive version is not an option

    Without a registered frame table, a throw does NOT degrade to a worse
    message. Measured on this tree: `vector::at(99)` in an image linked with
    the default script takes an unhandled CPU fault, dumps registers, and
    WEDGES the machine -- the firmware never regains control, so the only way
    out is a power cycle. That is strictly worse than the diagnostic the
    exceptions build prints, and it is why "just select the other linker
    script" is not a shippable answer on its own.

    An empty-but-valid `.eh_frame` (a lone CIE-list terminator, so
    `__register_frame` is handed a well-formed table with no FDEs) was tried
    and faults identically. The fault is inherent to unwinding a frame that
    has no FDE, not to the table being absent -- so the interception has to
    happen BEFORE the unwinder is entered, which is here.

    @par Why `--wrap` rather than defining the symbol

    Defining `__cxa_throw` outright collides: libsupc++'s `eh_throw.o` is
    pulled onto the link anyway, because axl-cxxrt-terminate.o's handler needs
    other symbols from it, and the result is `multiple definition of
    __cxa_throw`. `ld --wrap=__cxa_throw` is the linker's own mechanism for
    this -- every reference is redirected here, no symbol collides, and
    libsupc++'s original stays reachable as `__real___cxa_throw` if a future
    build ever wants both.

    A useful side effect: with nothing calling the real one, `--gc-sections`
    collects more of libsupc++'s throw path, so this build is slightly SMALLER
    than simply dropping the frame table.

    @par `what()` without an unwinder

    `__cxa_throw` receives the `std::type_info *` for the thrown type, which is
    enough to ask whether the object is catchable as a `std::exception`:
    `__do_catch` is `type_info`'s own virtual and needs no unwinder, no
    landing pad and no `dynamic_cast`. The call goes ON THE CATCH TYPE with the
    thrown type as the argument -- the other direction compiles, returns false
    for everything, and silently costs the `what()` line.

    This TU therefore compiles with RTTI (`CXXFLAGS_EH` already strips
    `-fno-rtti`, which is why it shares those flags with the terminate
    handler). Consumer translation units are unaffected and stay `-fno-rtti`.

    @par What a consumer gives up

    Real `try`/`catch`. A handler in a `--no-eh-frame` image can never run,
    because nothing unwinds to it -- so `axl-c++` REFUSES `--no-eh-frame`
    together with `-fexceptions` rather than producing an image whose handlers
    are silently dead. That refusal is the difference between this being a
    diagnostics trade and a correctness one.
**/

#include <exception>
#include <typeinfo>

extern "C" {

/* Declared here rather than through <axl.h>: this file is linked into images
   whose only AXL dependency is the runtime, and axl_printf is a macro for
   axl_print, which is the actual symbol. */
int  axl_print(const char *fmt, ...);
void axl_exit(int status);

/**
 * Report the throw and exit. Never returns, and never unwinds.
 *
 * Named `__wrap___cxa_throw` because the link passes `--wrap=__cxa_throw`;
 * the toolchain's own is then `__real___cxa_throw`, which nothing here calls.
 */
[[noreturn]] void
__wrap___cxa_throw(
    void           *thrown,   ///< the exception object
    std::type_info *tinfo,    ///< its type, from the throw site
    void          (*dest)(void *)  ///< its destructor
)
{
    /* Not run. The image is going down and the object's storage goes with it;
       calling a destructor here would be the only code between the throw and
       the exit that could itself fault. */
    (void)dest;

    axl_print("terminate: throw of type %s (image linked --no-eh-frame)\n",
              tinfo != nullptr ? tinfo->name() : "<unknown>");

    if (tinfo != nullptr) {
        /* __do_catch may ADJUST the pointer for a base-class match, which is
           why it takes `void **` -- a derived-to-base cast is not always the
           identity. Reading `p` afterwards rather than `thrown` is what makes
           the what() call land on the right subobject. */
        void *p = thrown;
        if (typeid(std::exception).__do_catch(tinfo, &p, 1)) {
            axl_print("  what(): %s\n",
                      static_cast<std::exception *>(p)->what());
        }
    }

    axl_exit(1);
    __builtin_unreachable();
}

} // extern "C"
