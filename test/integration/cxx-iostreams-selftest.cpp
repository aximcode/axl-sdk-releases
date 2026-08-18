/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-iostreams-selftest.cpp
    Fixture for test-cxx-iostreams-qemu.sh: the REAL `<iostream>`,
    `<sstream>` and `<fstream>` under UEFI, built with no mode flag.

    This is what P4 buys (`docs/AXL-Libc-Substrate-Design.md` §4d). Before
    it, a default C++ link carried `libaxl-cxx.a` and no libstdc++ at all,
    so `std::cout` was an undefined reference -- the AXL-side substitutes
    covered `operator new`, the `std::__throw_*` halts and a handful of
    container internals, and nothing else. Every stream below is a symbol
    that link could not resolve.

    THE CHAIN EACH VERB PROVES, because "iostreams work" is three separate
    claims that fail in different places:

      out   std::cout -> stdio_sync_filebuf -> newlib `stdout` -> fd 1 ->
            AXL's write() -> AxlStream console. The P2 porting layer is
            what the bottom half is; this asserts libstdc++ reaches it.
      sstr  std::ostringstream / istringstream -- no fd at all, but the
            locale and num_put machinery `AXL-Cxx-Stdlib-Surface.md` Tier 3
            claimed was blocked. P5 retracted that claim on the
            -fexceptions path; this pins it on the DEFAULT one.
      file  std::ofstream / ifstream round-trip on the ESP. `<fstream>`
            needs `open`, which AXL did not define at all before P2.

    `std::ios_base::Init` is a global constructor, so a run that prints
    nothing at all is the `.init_array` regression (`--gc-sections` ate it
    once already, silently, for as long as the C++ layer existed) rather
    than an iostreams bug. `ctor:` below is what tells those apart.

    Every line is asserted with grep -Fxq, so the text here IS the contract.
    Nothing prints a value it did not compute.
**/

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <axl.h>

/* Proof that the .init_array walker ran at all, independent of whether
   std::cout itself works: if this line is missing the failure is the
   constructor pass, not the stream. std::ios_base::Init is a global
   constructor too, so the two share a fate. */
static struct CtorProbe {
    CtorProbe(void) { axl_print("ctor: init_array ran\r\n"); }
} sCtorProbe;

static void
verb_out(void)
{
    std::cout << "out: text " << 42 << ' ' << -7 << ' ' << true << std::endl;

    /* Floating point is the half that needs num_put and the locale's
       numpunct facet -- the exact machinery Tier 3 said was unreachable. */
    std::cout << "out: float " << 3.5 << ' ' << -0.25 << std::endl;

    std::cout << "out: widths " << 4000000000u << ' ' << -9000000000LL
              << ' ' << 18000000000ULL << std::endl;

    /* std::string through the stream, not through c_str(): operator<< for
       basic_string is a separate out-of-line symbol from the char* one. */
    std::string s("from-a-std-string");
    std::cout << "out: " << s << " len=" << s.size() << std::endl;

    std::cerr << "err: to cerr" << std::endl;

    /* One LF must reach the wire as ONE CRLF. std::endl writes '\n' by
       definition, so what this actually exercises is AXL's own console
       transcode (console_transcode_crlf in src/stream/axl-stream.c) driven
       from libstdc++'s stdio_sync_filebuf -- a double-translation there puts
       \r\r\n on the wire.

       The harness asserts this on the RAW serial log, not the cleaned one:
       it runs `tr -d '\r'`, which deletes ALL carriage returns, so \r\r\n
       and \r\n both arrive as the same line and the grep below cannot tell
       them apart. An earlier version of this comment claimed exactly that
       guard and did not have it. */
    std::cout << "out: endl-once" << std::endl;
}

static void
verb_sstream(void)
{
    std::ostringstream os;
    os << "n=" << 42 << " f=" << 1.5;
    std::cout << "sstr: built " << os.str() << std::endl;

    std::istringstream is("7 hello 2.5");
    int         n = 0;
    std::string word;
    double      d = 0;
    is >> n >> word >> d;
    std::cout << "sstr: parsed " << n << ' ' << word << ' ' << d
              << " ok=" << (bool) is << std::endl;
}

static void
verb_file(void)
{
    const char *path = "fs0:\\axl-iostreams-test.txt";

    {
        std::ofstream out(path);
        if (!out) {
            std::cout << "file: OPEN-FAILED write" << std::endl;
            return;
        }
        out << "line-one 123" << std::endl;
        out << "line-two 456" << std::endl;
    }

    std::ifstream in(path);
    if (!in) {
        std::cout << "file: OPEN-FAILED read" << std::endl;
        return;
    }

    std::string first;
    std::string second;
    std::getline(in, first);
    std::getline(in, second);
    std::cout << "file: read [" << first << "][" << second << "]" << std::endl;

    /* Re-read as fields, so the round-trip proves content and not just
       byte count. */
    std::ifstream again(path);
    std::string   word;
    int           value = 0;
    again >> word >> value;
    std::cout << "file: fields " << word << ' ' << value << std::endl;

    remove(path);
}

int
main(void)
{
    verb_out();
    verb_sstream();
    verb_file();

    std::cout << "iostreams: done" << std::endl;
    return 0;
}
