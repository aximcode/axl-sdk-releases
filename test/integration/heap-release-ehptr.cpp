/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-ehptr.cpp — exception storage in a --no-eh-frame image carries
 * the ABI header, for test-heap-release-qemu.sh. Built `--no-eh-frame -frtti`.
 *
 * axl-cxxrt-nothrow.o replaces libsupc++'s eh_alloc.o in such an image (no
 * emergency pool). With RTTI on, std::make_exception_ptr is the REAL one even
 * under -fno-exceptions: it calls __cxa_allocate_exception and then libsupc++'s
 * __cxa_init_primary_exception, which writes the 128-byte
 * __cxa_refcounted_exception header in front of the object, and
 * ~exception_ptr reads its refcount back. Storage without that header has the
 * write land on whatever precedes the block.
 *
 * A sentinel block is allocated first; dlmalloc carves the exception's
 * storage right after it, so a header written outside the allocation lands in
 * the sentinel's tail. Prints
 *
 *   EHPTR ok        every sentinel byte intact, the pointer round-tripped
 *   EHPTR corrupt   a write landed outside the allocation
 */

#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

#include <axl.h>

int
main()
{
    constexpr std::size_t kSentinel = 512;
    auto *sentinel = static_cast<unsigned char *>(std::malloc(kSentinel));

    if (sentinel == nullptr) {
        return 1;
    }
    std::memset(sentinel, 0x5A, kSentinel);

    bool round_trip = true;

    for (int i = 0; i < 8; i++) {
        std::exception_ptr p =
            std::make_exception_ptr(std::runtime_error("ehptr"));
        std::exception_ptr q = p;   // refcount 2, read back on each release
        round_trip = round_trip && p != nullptr && p == q;
    }

    bool intact = true;

    for (std::size_t i = 0; i < kSentinel; i++) {
        intact = intact && sentinel[i] == 0x5A;
    }
    std::free(sentinel);

    axl_printf("EHPTR %s\n", intact && round_trip ? "ok" : "corrupt");
    return 0;
}
