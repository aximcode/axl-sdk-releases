/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-streams.cpp
    axl::cout / axl::cin / axl::cerr and axl::string.

    `axl::string` predates the retirement of the freestanding C++ mode, when
    `<string>` was gated behind `bits/requires_hosted.h` and a translation
    unit had no owning string at all. `std::string` is always available now,
    so `axl::string` is a choice rather than a necessity: it is the smaller
    one, and it converts at the seam.

    Build and run:

        axl-c++ cxx-streams.cpp -o cxx-streams.efi

        # interactive
        fs0:\> cxx-streams.efi

        # or feed it -- a redirect and either pipe form all work
        fs0:\> cxx-streams.efi < input.txt
        fs0:\> echo "8080 example.com" | cxx-streams.efi
**/

#include <stdint.h>

#include <axl/axl-istream.hpp>
#include <axl/axl-ostream.hpp>
#include <axl/axl-string.hpp>

int
main(void)
{
    axl::cout << "axl-sdk C++ streams" << axl::endl;

    // ---------------------------------------------------------------
    // axl::string -- std::string's interface, AxlString's buffer.
    // ---------------------------------------------------------------
    axl::string greeting("hello");
    greeting += ", world";
    greeting.push_back('!');

    axl::cout << greeting << " (" << (int)greeting.size() << " bytes)"
              << axl::endl;

    // The search family forwards to std::string_view, which IS freestanding
    // -- so these are libstdc++'s own algorithms, not reimplementations.
    if (greeting.contains("world")) {
        axl::cout << "  found 'world' at " << (int)greeting.find("world")
                  << axl::endl;
    }

    // ---------------------------------------------------------------
    // Reading. Two spellings, one sticky state -- mix them freely.
    // ---------------------------------------------------------------
    axl::cout << "port and host? ";

    uint16_t    port = 0;
    axl::string host;
    axl::cin >> port >> host;

    if (!axl::cin) {
        // The chained form: one check covers the whole run.
        axl::cerr << "could not read a port and a host (status "
                  << (int)axl::cin.status() << ")" << axl::endl;
        return 1;
    }

    axl::cout << "  host=" << host << " port=" << port << axl::endl;

    // The checked form: the failure arrives as a value to branch on, which
    // is the same shape axl::result gives the rest of the C++ layer.
    axl::cout << "retries? ";
    axl::result<uint32_t> retries = axl::cin.read<uint32_t>();
    if (retries) {
        axl::cout << "  retries=" << *retries << axl::endl;
    } else {
        axl::cout << "  no retry count given, defaulting to 3" << axl::endl;
        axl::cin.clear();       // forget the failure and carry on
    }

    // Whole lines, when whitespace is part of the value.
    axl::cout << "description? ";
    axl::string description;
    axl::getline(axl::cin, description);
    if (axl::cin) {
        axl::cout << "  description='" << description << "'" << axl::endl;
    }

    return 0;
}
