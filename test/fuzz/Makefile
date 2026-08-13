# test/fuzz/Makefile
#
# Standalone host-side libFuzzer build for AXL parser harnesses.
# NOT wired into the top-level Makefile — fuzzing is opt-in and
# requires a clang toolchain with libFuzzer + AddressSanitizer.
#
# Usage:
#   make -C test/fuzz                   # build all harnesses
#   ./test/fuzz/url_fuzz url_corpus/    # run against seed corpus
#
# The fuzz harness compiles AXL parser sources directly against a
# tiny libc-backed shim (fuzz_shim.c) rather than linking the
# freestanding library, so it can be instrumented with ASan.

# Require clang — libFuzzer is clang-only. Users can override with
# `make CC=/path/to/clang` but the default `cc` from Make's builtin
# rules won't work, so we replace it when it hasn't been set explicitly.
ifeq ($(origin CC),default)
CC := clang
endif

REPO    := ../..
# -DAXL_ALLOW_UEFI: the harnesses compile AXL's own sources, which reach
# <uefi/axl-uefi.h> through the backend. That header refuses to compile without
# this define so that consumer APPLICATIONS cannot acquire EDK2 by including
# it; the library and its test infrastructure are on the other side of that
# line. Concretely it is src/data/axl-str.c that reaches the header, so the two
# harnesses taking AXL_BASE (url_fuzz, json_fuzz) fail without this.
CFLAGS  := -O1 -g -fsanitize=fuzzer,address -fno-omit-frame-pointer \
           -DAXL_ALLOW_UEFI \
           -I$(REPO)/include -I$(REPO)/src/data
LDFLAGS := -fsanitize=fuzzer,address

HARNESSES := url_fuzz json_fuzz ipmi_fuzz

#
# AXL_BASE — the string/format substrate every harness that touches AXL
# string code needs. ipmi_fuzz is the exception and deliberately does NOT
# take it: its response decoders reference no AXL string symbol, so it still
# builds from its own two sources plus the shim.
#
# This list is the transitive closure of what the parser sources reference,
# established by linking and following the undefined symbols, not guessed.
# It is only six files, which is worth stating because the note this replaces
# claimed the closure was "most of the library" and used that as the reason
# json_fuzz could not be fixed. It is not: no backend, no axl-mem.c, no
# axl-fs, no event loop. axl-str.c pulls axl-str-scan.c and axl-format.c;
# axl-format.c pulls the float formatters; axl-strtod.c pulls axl-math.c.
#
# These are compiled from SOURCE rather than linked from libaxl.a on purpose,
# and libaxl.a is not an option: it is built freestanding for the UEFI ABI,
# so it neither links into a host binary nor carries ASan instrumentation.
# Compiling here gets both, and puts the string code under the sanitizer
# alongside the parser that calls it.
#
# When a parser grows a dependency this list does not cover, the harness
# fails to LINK. That is the intended failure mode, and `make check-fuzz-link`
# in the top-level Makefile is what makes anyone notice.
#
AXL_BASE := $(REPO)/src/data/axl-str.c \
            $(REPO)/src/data/axl-str-scan.c \
            $(REPO)/src/format/axl-format.c \
            $(REPO)/src/format/axl-strtod.c \
            $(REPO)/src/format/axl-dtoa.c \
            $(REPO)/src/math/axl-math.c

all: $(HARNESSES)

#
# url_fuzz: compiles axl-url.c + AXL_BASE + fuzz_shim.c + url_fuzz.c.
#
# It used to build against libc wrappers for the string helpers. It now gets
# the real axl-str.c, so the string code axl_url_parse leans on is fuzzed and
# ASan-instrumented rather than substituted away.
#
url_fuzz: url_fuzz.c fuzz_shim.c $(REPO)/src/net/axl-url.c $(AXL_BASE)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

#
# json_fuzz: the accessors (axl-json-parse.c) + the parser (axl-json-lex.c)
# + duplicate-key detection (axl-hash-table.c, which pulls axl-list.c for its
# bucket chains) + AXL_BASE. Both JSON sources include axl-json-internal.h by
# relative path, which is why -I$(REPO)/src/data is in CFLAGS.
#
# This target was broken for months and is now fixed. Two things had to be
# true at once, which is why naming the missing objects never closed it:
# axl-str.c had to be compiled in (for axl_memchr / axl_memmove /
# axl_utf8_decode), AND the seven libc wrappers it collided with had to come
# OUT of fuzz_shim.c. Fixing only one of those trades undefined symbols for
# duplicate ones.
#
# The cost of it having been broken: ASan/LSan coverage of the JSON read path
# was zero for the whole flag redesign, which is why two OOM defects in that
# path were caught by code review rather than by tooling.
#
#
# BOTH directions of the JSON API, which is the point of the list below.
#
# json_fuzz used to compile the reader and nothing else, so the WRITER --
# axl-json-build.c, where axl_json_writer_init / write_token / finish live
# along with every formatting flag P5-P8 added -- had no fuzz coverage at all,
# and neither did P10's source/sink layer in axl-json-io.c. Reading only also
# meant the harness could not have an ORACLE: with no way to re-serialize, the
# only detectable defect was a crash, and this code's characteristic bug is
# returning the WRONG BYTES without crashing.
#
# axl-json-print.c is deliberately NOT here: it holds exactly one function,
# axl_json_console_print, which writes through the UEFI console backend. That
# backend does not build for the host, and a console pretty-printer is not
# reachable from a byte-buffer harness anyway.
#
# The rest are what the two directions drag in: hash-table (+list) for
# REJECT_DUPLICATES, array + util/axl-sort.c for SORT_KEYS, string for the
# writer's AxlString sink.
#
JSON_SRCS := $(REPO)/src/data/axl-json-parse.c \
             $(REPO)/src/data/axl-json-lex.c \
             $(REPO)/src/data/axl-json-build.c \
             $(REPO)/src/data/axl-json-io.c \
             $(REPO)/src/data/axl-hash-table.c \
             $(REPO)/src/data/axl-list.c \
             $(REPO)/src/data/axl-array.c \
             $(REPO)/src/data/axl-string.c \
             $(REPO)/src/util/axl-sort.c

json_fuzz: json_fuzz.c fuzz_shim.c $(JSON_SRCS) $(AXL_BASE)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

#
# ipmi_fuzz: compiles axl-ipmi-cmd.c + axl-ipmi-format.c + fuzz_shim.c
# + ipmi_fuzz.c. The fuzz target overrides axl_ipmi_raw() at link
# time so the full src/ipmi/axl-ipmi.c (session, transports,
# backend-dependent auto-detect) is NOT compiled — just the response
# decoders + string tables. cmd.c was refactored to drop its
# axl-ipmi-internal.h include specifically to keep this build clean.
#
ipmi_fuzz: ipmi_fuzz.c fuzz_shim.c \
           $(REPO)/src/ipmi/axl-ipmi-cmd.c \
           $(REPO)/src/ipmi/axl-ipmi-format.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(HARNESSES) *.o
	rm -rf crash-* leak-* oom-*

.PHONY: all clean
