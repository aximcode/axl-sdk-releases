# test/fuzz -- host-side libFuzzer harnesses for AXL's parsers (the `fuzz`
# preset; root CMakeLists.txt builds ONLY this directory when AXL_FUZZ is on).
#
# Not part of the UEFI build: libFuzzer and AddressSanitizer need a host clang,
# and the freestanding cross toolchain cannot use them. Each harness compiles
# the AXL parser sources it fuzzes directly, against a small libc-backed shim
# (fuzz_shim.c), rather than linking libaxl.a -- which is built freestanding
# for the UEFI ABI, so it neither links into a host binary nor carries ASan
# instrumentation. Compiling here gets both, and puts the string code under
# the sanitizer alongside the parser that calls it.
#
#   cmake --preset fuzz && cmake --build --preset fuzz && ctest --preset fuzz
#
# ctest replays every committed seed corpus at -runs=0 (deterministic, no
# mutation, well under a second): "it links, and the seeds are still clean
# under ASan". Fuzzing for real stays opt-in -- see README.md.

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "fuzz: libFuzzer is clang-only; ${CMAKE_C_COMPILER} is "
        "${CMAKE_C_COMPILER_ID}. Configure with -DCMAKE_C_COMPILER=<clang>.")
endif()

set(_r ${CMAKE_SOURCE_DIR})

# -DAXL_ALLOW_UEFI: the harnesses compile AXL's own sources, which reach
# <uefi/axl-uefi.h> through the backend (src/data/axl-str.c does). That header
# refuses to compile without it, so that consumer APPLICATIONS cannot acquire
# EDK2 by including it; the library's test infrastructure is on the other
# side of that line. src/data is on the path because both JSON sources include
# axl-json-internal.h by relative path.
add_library(axl_fuzz_flags INTERFACE)
target_compile_options(axl_fuzz_flags INTERFACE
    -O1 -g -fsanitize=fuzzer,address -fno-omit-frame-pointer)
target_compile_definitions(axl_fuzz_flags INTERFACE AXL_ALLOW_UEFI)
target_include_directories(axl_fuzz_flags INTERFACE ${_r}/include ${_r}/src/data)
target_link_options(axl_fuzz_flags INTERFACE -fsanitize=fuzzer,address)

# AXL_BASE -- the string/format substrate every harness touching AXL string
# code needs: the transitive closure of what the parser sources reference,
# established by linking and following the undefined symbols, not guessed.
# No backend, no axl-mem.c, no axl-fs, no event loop: axl-str.c pulls
# axl-str-scan.c and axl-format.c, axl-format.c the float formatters,
# axl-strtod.c axl-math.c. A parser growing a dependency this does not cover
# fails to LINK -- the intended failure, and the fuzz gate is what notices.
set(AXL_FUZZ_BASE
    ${_r}/src/data/axl-str.c
    ${_r}/src/data/axl-str-scan.c
    ${_r}/src/format/axl-format.c
    ${_r}/src/format/axl-strtod.c
    ${_r}/src/format/axl-dtoa.c
    ${_r}/src/math/axl-math.c)

# url_fuzz: the URL parser over the REAL string code (not libc stand-ins), so
# what axl_url_parse leans on is fuzzed too.
set(AXL_FUZZ_SRCS_url ${_r}/src/net/axl-url.c ${AXL_FUZZ_BASE})

# json_fuzz: BOTH directions of the JSON API. The reader alone had no ORACLE
# -- with no way to re-serialize, the only detectable defect was a crash, and
# this code's characteristic bug is returning the WRONG BYTES. So: the
# accessors and parser, the writer (axl-json-build.c) and the source/sink
# layer (axl-json-io.c), plus what they drag in -- hash-table (+list) for
# REJECT_DUPLICATES, array + axl-sort.c for SORT_KEYS, string for the writer's
# sink. axl-json-print.c is deliberately absent: its one function writes
# through the UEFI console backend, which does not build for the host.
set(AXL_FUZZ_SRCS_json
    ${_r}/src/data/axl-json-parse.c
    ${_r}/src/data/axl-json-lex.c
    ${_r}/src/data/axl-json-build.c
    ${_r}/src/data/axl-json-io.c
    ${_r}/src/data/axl-hash-table.c
    ${_r}/src/data/axl-list.c
    ${_r}/src/data/axl-array.c
    ${_r}/src/data/axl-string.c
    ${_r}/src/util/axl-sort.c
    ${AXL_FUZZ_BASE})

# ipmi_fuzz: the response decoders and string tables only. The harness
# overrides axl_ipmi_raw() at link time, so axl-ipmi.c (sessions, transports,
# backend auto-detect) is NOT compiled -- and it needs no AXL string code.
set(AXL_FUZZ_SRCS_ipmi ${_r}/src/ipmi/axl-ipmi-cmd.c ${_r}/src/ipmi/axl-ipmi-format.c)

# format_fuzz: the printf engine itself, over a payload in an EXACTLY-SIZED
# heap block so ASan owns the byte past its end. AXL_BASE is its whole
# dependency (axl-format.c allocates and logs nothing).
set(AXL_FUZZ_SRCS_format ${AXL_FUZZ_BASE})

foreach(_h url json ipmi format)
    add_executable(${_h}_fuzz ${_h}_fuzz.c fuzz_shim.c ${AXL_FUZZ_SRCS_${_h}})
    target_link_libraries(${_h}_fuzz PRIVATE axl_fuzz_flags)
    # build/fuzz/<h>_fuzz, where README.md's commands run them from.
    set_target_properties(${_h}_fuzz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    # The gate: the committed seeds replay clean under ASan. -artifact_prefix
    # keeps a failing run's reproducer in the BUILD tree -- libFuzzer otherwise
    # drops it in the current directory, where `git add` would find it.
    add_test(NAME fuzz.seeds.${_h}
        COMMAND ${_h}_fuzz -runs=0 -artifact_prefix=${CMAKE_BINARY_DIR}/artifacts/
                ${CMAKE_CURRENT_SOURCE_DIR}/${_h}_corpus/)
    set_tests_properties(fuzz.seeds.${_h} PROPERTIES LABELS "gate;fuzz"
        # A corpus libFuzzer cannot read is not a pass: it reports what it
        # replayed, and zero inputs means nothing was checked.
        FAIL_REGULAR_EXPRESSION "INFO: +0 files found|ERROR: The required directory")
endforeach()
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/artifacts)
