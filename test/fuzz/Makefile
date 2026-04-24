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
CFLAGS  := -O1 -g -fsanitize=fuzzer,address -fno-omit-frame-pointer \
           -I$(REPO)/include
LDFLAGS := -fsanitize=fuzzer,address

HARNESSES := url_fuzz json_fuzz ipmi_fuzz

all: $(HARNESSES)

#
# url_fuzz: compiles axl-url.c + fuzz_shim.c + url_fuzz.c.
# axl-url.c only depends on axl-mem.h / axl-str.h / axl-log.h / axl-url.h,
# all shimmed in fuzz_shim.c.
#
url_fuzz: url_fuzz.c fuzz_shim.c $(REPO)/src/net/axl-url.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

#
# json_fuzz: compiles axl-json-parse.c + fuzz_shim.c + json_fuzz.c.
# axl-json-parse.c vendors jsmn.h from src/data/ via a relative
# include, so we pass -I for that directory too.
#
json_fuzz: json_fuzz.c fuzz_shim.c $(REPO)/src/data/axl-json-parse.c
	$(CC) $(CFLAGS) -I$(REPO)/src/data $^ -o $@ $(LDFLAGS)

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
