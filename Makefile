# Makefile — Build AXL library and tests (GCC toolchain)
#
# Usage:
#   make                          # build libaxl.a (x64)
#   make ARCH=aa64                # cross-compile for aarch64
#   make tests                    # build all test EFIs
#   make tools                    # build standalone tool EFIs
#   make clean

ARCH       ?= x64
# Guard: only x64 / aa64 are valid. The toolchain block below is `ifeq aa64 ...
# else <x64>`, so any other value (a typo, or run-qemu's uppercase ARCH=X64
# passed straight to make) silently builds with the x64 toolchain into
# out/native-<wrong>/ and links against absent libs. That masked a
# clean-CI-only break behind a passing local run (the .efi already existed).
# Fail loudly and consistently instead.
ifeq ($(filter $(ARCH),x64 aa64),)
  $(error invalid ARCH '$(ARCH)' -- must be 'x64' or 'aa64')
endif
TYPE       ?= app
BUILD      ?= DEBUG

# Each BUILD gets its OWN output tree. Objects are flag-dependent and make
# cannot tell that a .o was compiled with different CFLAGS, so sharing one
# directory silently mixes them: a `BUILD=RELEASE` bench run followed by a
# plain `make tests` links DEBUG-only tests against a RELEASE libaxl.a and
# reports failures that are pure artefact (`debug: alloc fill 0xDA` is the
# one that bites, since AXL_MEM_DEBUG is off in RELEASE). `make tests` does
# NOT recover it either — the objects look up to date — so the only fix used
# to be remembering `make clean` between modes. Now the two trees coexist.
# The default build keeps the historical path, so every script and doc that
# names out/native-<arch> is unaffected; `make print-prefix` reports it for
# anything that needs to find the other one.
# AXL_TLS splits the tree too, for the same reason BUILD does and with a
# sharper edge. It is in the build-state SIGNATURE (see TLS_STATE), so a toggle
# WIPES $(BUILDDIR)/*.o, libaxl.a and every .efi under the prefix. The toggle
# is not hypothetical: test-axl.sh builds with AXL_TLS off, run-integration.sh
# exports AXL_TLS=1, and a developer's bare `make` is off again. Sharing one
# prefix therefore wiped and rebuilt on every alternation -- measured at 321
# objects one way and 270 back -- and made running two of them CONCURRENTLY
# corrupt both, same prefix, two writers.
#
# The suffix derives from the same `$(if $(AXL_TLS),...)` predicate that feeds
# TLS_STATE and LIB_SOURCES, not a second spelling of it: a prefix that could
# disagree with which sources compiled would be worse than sharing one. Note
# `ifdef`/`$(if ...)` treat AXL_TLS=0 as ON; that is pre-existing semantics,
# and deriving from the same expression keeps them consistent.
#
# out/native-x64-tls already existed by hand, in two integration scripts that
# had worked this out locally and set PREFIX= themselves; their comments gave
# the same rationale. This generalises it and those overrides are now gone.
#
# Every consumer must ASK for the prefix rather than compose it --
# `make -s print-prefix`, or test_build_dir/test_build_prefix in
# common-test.sh. A first attempt at this split shipped without that and broke
# 48 integration suites on paths like "$PROJECT_DIR/out/native-x64/tools/...".
AXL_TLS_SUFFIX := $(if $(AXL_TLS),-tls,)
ifeq ($(BUILD),DEBUG)
  PREFIX   ?= out/native-$(ARCH)$(AXL_TLS_SUFFIX)
else
  PREFIX   ?= out/native-$(ARCH)-$(shell echo $(BUILD) | tr '[:upper:]' '[:lower:]')$(AXL_TLS_SUFFIX)
endif
HOSTCC     ?= gcc
AXL_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)

# ===================================================================
# Toolchain: gcc
# ===================================================================

ifeq ($(ARCH),aa64)
  # OUR toolchain's binutils, not apt's aarch64-linux-gnu-*. ARM's ships
  # pei-aarch64-little, so the .so -> .efi objcopy works with it; the hermetic
  # direction (AXL-Libc-Substrate-Design.md §4.1d) says nothing comes from the
  # host. x64 still cannot follow: its binutils has no PE backend until the
  # toolchain is rebuilt with --enable-targets=x86_64-pep.
  CROSS      = $(or $(AXL_AA64_BINUTILS_PREFIX),$(AXL_AA64_BINUTILS_PREFIX_DEFAULT))
  # -mno-outline-atomics: keep atomic ops inline (avoids unresolved
  # __aarch64_ldadd* helper-function references in freestanding builds).
  # -ffixed-x18: UEFI AArch64 binding (UEFI 2.11 §2) reserves x18 as
  # the platform register; gcc's register allocator must not use it
  # or post-ExitBootServices OS-side state gets corrupted. Library
  # objects must respect this just as much as user code, since library
  # functions called from a user app would otherwise clobber x18.
  GCC_ARCH   = -mno-outline-atomics -ffixed-x18
  EFI_LDS    = scripts/elf_aarch64_efi.lds
  PE_TARGET  = pei-aarch64-little
else
  # OURS now (the -axl toolchain adds the PE target objcopy needs); see the
  # aa64 branch above and AXL-Libc-Substrate-Design.md §4.1d.
  CROSS      = $(or $(AXL_X64_BINUTILS_PREFIX),$(AXL_X64_BINUTILS_PREFIX_DEFAULT))
  GCC_ARCH   = -mno-red-zone -march=x86-64
  EFI_LDS    = scripts/elf_x86_64_efi.lds
  PE_TARGET  = pei-x86-64
endif

# C compiles with the BARE-METAL cross for both arches, not with host gcc
# (x64) or the Linux cross (aa64). Both of those resolve <string.h> and friends
# to a hosted libc's headers -- /usr/include on x64, the glibc-targeted cross's
# on aa64 -- and this build uses no host headers or libraries. That is what
# retired include/compat: the third-party code in deps/ is the only thing that
# ever asked for those headers, and newlib supplies the genuine article.
# docs/AXL-Libc-Substrate-Design.md §4.1b.
#
# Binutils stay as they were ($(CROSS)ld / ar / objcopy). They consume objects,
# not headers, so they are not part of the hermetic question -- and the flip was
# measured with exactly this pairing: 271/271 objects and 43 images per arch,
# AxlTestData 2078/0 on both.
ifeq ($(ARCH),aa64)
  CC       = $(or $(AXL_AA64_GCC),$(AXL_AA64_GCC_DEFAULT))
else
  CC       = $(or $(AXL_X64_GCC),$(AXL_X64_GCC_DEFAULT))
endif
LD_ELF     = $(CROSS)ld
AR         = $(CROSS)ar
OBJCOPY    = $(CROSS)objcopy

# C++ toolchain.  Bare-metal on BOTH arches now, matching $(CC) above.
# AArch64 uses ARM's "none-elf" cross because the Linux-ABI cross's libstdc++
# headers pull hosted typedefs from <bits/c++config.h> (see docs/ROADMAP.md
# axlmm spec).  X64 uses AXL's own x86_64-elf toolchain, which nobody
# publishes -- toolchain/x86_64-elf/build-toolchain.sh builds it.
#
# X64 used to be the HOST g++, and dropping that was the last host input the
# SDK had (AXL-Libc-Substrate-Design.md §4.1d).  It is not only a hermeticity
# argument: the host compiler is glibc-targeted, so its libsupc++ reads the
# exception globals and the stack canary through %fs, which UEFI never sets
# up.  See AXL-Cxx-Design.md §6a-PLAN task T2.
#
# CXX and the C++ flags are set UNCONDITIONALLY, not under `ifdef AXL_CPP`.
# They used to be guarded, and the consequence was not that the C++ gates were
# skipped -- it was that they ran with the wrong compiler and NO flags.
# check-examples and check-cxx-entry both expand `$(CXX) $(CXXFLAGS_BASE)`,
# and verify.sh and CI invoke them without AXL_CPP=1, so CXX fell back to
# make's builtin `g++` and CXXFLAGS_BASE expanded to nothing. Every C++
# example was therefore compiled HOSTED, at gnu++17, with exceptions and RTTI
# enabled and without $(GCC_ARCH) -- i.e. under none of the constraints a
# consumer actually gets, by the gate whose entire job is to prove they hold.
#
# The `ifdef` still guards what it should: a hard error when a real C++ BUILD
# is asked for and the toolchain is missing.
# Toolchain locations live in ONE file, shared verbatim with scripts/axl-cc
# and the generated axl-config.cmake -- see scripts/axl-toolchains.conf for why
# its KEY=VALUE subset is both valid make and valid sh. `-include` rather than
# `include` so the lint gates and `make clean` still work in a tree without it;
# a real build without it now fails at the CC guard below, since C has no host
# fallback to land on.
-include $(dir $(lastword $(MAKEFILE_LIST)))scripts/axl-toolchains.conf

# AXL_TOOLCHAIN -- WHICH toolchain supplies the compiler and binutils.
#
#   axl    (default) the ones scripts/axl-toolchains.conf names and
#          scripts/install-toolchain.sh installs.
#   cross  one you supply, named by AXL_<ARCH>_GCC / _GXX / _BINUTILS_PREFIX.
#          The conf defaults are NOT consulted.
#
# A VARIANT beside the LOCATORS, because a locator alone cannot express which
# toolchain you meant. That is the convention everywhere this problem is
# solved: Zephyr pairs ZEPHYR_TOOLCHAIN_VARIANT with ZEPHYR_SDK_INSTALL_DIR /
# CROSS_COMPILE, EDK2 pairs TOOL_CHAIN_TAG with GCC_AARCH64_PREFIX, the Linux
# kernel pairs LLVM=1 with CROSS_COMPILE. None selects a toolchain by bare
# prefix alone.
#
# The bug it closes is ours: `make CROSS=<prefix>-` was documented as the macOS
# and native-Windows build, and has selected only BINUTILS since 0bf6ed51
# replaced `CC = $(CROSS)gcc` with an AXL_*_GCC lookup. The compiler stayed
# pointed at /opt, so the command could not work on a host with no /opt
# toolchain -- and it died in a recipe rather than where the intent was stated.
#
# `cross` deliberately does NOT fall back to the defaults. Falling back is what
# makes the failure late; refusing with the variable names is the feature.
#
# `llvm` is deliberately ABSENT rather than accepted-and-ignored. It is the
# obvious next value, and it is not a stub: clang is one binary carrying every
# backend, so it is selected by --target= rather than by a prefix -- which is
# why the kernel's LLVM=1 reinterprets CROSS_COMPILE as a triple instead of a
# binary prefix. A variant that silently did nothing would be worse than one
# that refuses.
AXL_TOOLCHAIN ?= axl

# The arch's spelling in the variable names, for diagnostics that can name the
# exact variable to set rather than a pattern the reader has to instantiate.
ifeq ($(ARCH),aa64)
  TC_ARCH := AA64
else
  TC_ARCH := X64
endif

ifeq ($(filter $(AXL_TOOLCHAIN),axl cross),)
  $(error AXL_TOOLCHAIN=$(AXL_TOOLCHAIN) is not a toolchain variant. Use `axl` \
    (the default: the toolchains scripts/install-toolchain.sh installs) or \
    `cross` (one you supply, named by AXL_$(TC_ARCH)_GCC, AXL_$(TC_ARCH)_GXX \
    and AXL_$(TC_ARCH)_BINUTILS_PREFIX))
endif

# `cross` means YOU name it, so the defaults stop existing. Clearing them here
# rather than editing the six $(or ...) sites keeps one definition of the
# fallback rule -- and means a locator left unset resolves EMPTY and is caught
# by the CC guard below, which is where the diagnostic lives.
ifeq ($(AXL_TOOLCHAIN),cross)
  # `override`, on every one of these, because a plain assignment CANNOT clear
  # a variable set on the make command line -- `make AXL_TOOLCHAIN=cross
  # ARM_TOOLCHAIN=/opt/...` would silently keep the fallback, which is the very
  # case this block exists to stop. (The same precedence rule made a sabotage
  # of this block read as undetected until it was written with `override`.)
  override AXL_X64_GCC_DEFAULT :=
  override AXL_X64_GXX_DEFAULT :=
  override AXL_X64_BINUTILS_PREFIX_DEFAULT :=
  override AXL_AA64_GCC_DEFAULT :=
  override AXL_AA64_GXX_DEFAULT :=
  override AXL_AA64_BINUTILS_PREFIX_DEFAULT :=
  # ARM_TOOLCHAIN is a DEFAULT by another name: the older override spelling
  # supplies the aa64 C++ compiler as `$(ARM_TOOLCHAIN)/bin/...-g++` when
  # AXL_AA64_GXX is unset. Leaving it live would let a stale environment
  # variable quietly provide a compiler under the one variant whose entire
  # promise is that nothing arrives implicitly -- so it is cleared too, and
  # `ifdef` reads an empty value as undefined.
  override ARM_TOOLCHAIN :=
endif

ifeq ($(ARCH),aa64)
  # ARM_TOOLCHAIN stays supported as the older override spelling; AXL_AA64_GXX
  # is the one axl-cc and install.sh honour, so both resolve here.
  # $(or ...) not ?=, so an EMPTY override falls back rather than yielding an
  # empty CXX. `?=` counts an environment-defined empty string as defined.
  ifdef ARM_TOOLCHAIN
    CXX = $(or $(AXL_AA64_GXX),$(ARM_TOOLCHAIN)/bin/aarch64-none-elf-g++)
  else
    CXX = $(or $(AXL_AA64_GXX),$(AXL_AA64_GXX_DEFAULT))
  endif
else
  CXX = $(or $(AXL_X64_GXX),$(AXL_X64_GXX_DEFAULT))
endif

ifdef AXL_CPP
  # Check $(CXX), NOT g++. The default is a bare-metal toolchain path now, so
  # testing for host g++ would pass on a box that has one and lacks the other
  # -- and the build then dies deep in a recipe with a bare "No such file or
  # directory" instead of here, which is the guard's only job. `command -v`
  # still covers an AXL_*_GXX override spelled as a bare name on PATH;
  # wildcard covers a path.
  ifeq ($(or $(wildcard $(CXX)),$(shell command -v $(CXX) 2>/dev/null)),)
    # Same two-message split as the C guard below, and for the same reason. It
    # also fixes a message that read "needs " with NOTHING after it: under
    # `cross` with the locator unset, $(CXX) is empty, so the old text named no
    # compiler at all and then recommended an installer the caller had already
    # declined by choosing this variant.
    ifeq ($(AXL_TOOLCHAIN),cross)
      $(error AXL_CPP=1 with AXL_TOOLCHAIN=cross needs AXL_$(TC_ARCH)_GXX set \
        to your own bare-metal C++ compiler$(if $(CXX), (it resolved to \
        "$(CXX)"), (it is unset)). The axl-toolchains.conf defaults are \
        deliberately not consulted under this variant)
    else
      $(error AXL_CPP=1 needs $(if $(CXX),$(CXX),a C++ compiler) — run \
        ./scripts/install-toolchain.sh $(ARCH))
    endif
  endif
endif

# Common flags for the EFI link step. The .so we produce here is just
# an intermediate consumed by objcopy → PE/COFF; no OS ever loads it,
# so the linker's RWX-segment warning is a false positive (the resulting
# .efi has properly split per-section permissions, see PE characteristics).
# --gc-sections drops unreferenced .text/.data sections from each linked
# .efi. Combined with -ffunction-sections / -fdata-sections in CFLAGS, this
# gives per-symbol selective linking: a tool that uses 5% of libaxl.a
# carries only that 5% in its .efi. Disabling this would balloon every
# tool binary by 50–80% (each .o member of libaxl.a brought in for one
# referenced symbol pulls every other symbol in that .o along with it).
# --version-script localizes every symbol but the entry point. Without it
# `-shared` exports all globals into the dynamic symbol table and treats them
# as --gc-sections roots, defeating dead-code elimination (a hello-world pulled
# the whole ~110 KB runtime). A UEFI image needs no exports, so localizing lets
# --gc-sections drop unreferenced code — ~66% smaller .efi. See efi-localize.ver.
EFI_VERSION_SCRIPT = scripts/efi-localize.ver
LDFLAGS_EFI = -nostdlib -shared -Bsymbolic --no-warn-rwx-segments --no-undefined \
              --gc-sections --version-script=$(EFI_VERSION_SCRIPT)

# Stack-smashing detection. `-mstack-protector-guard=global` is the load-bearing
# half on x86-64: GCC otherwise reads the canary from %fs:0x28, glibc's TLS
# block, which UEFI never sets up -- so the default form faults instead of
# protecting, and that is why this was off. AArch64 already defaults to the
# global symbol. src/runtime/axl-stack-guard.c supplies the value and handler.
#
# -strong rather than -all: -all protects every function with any local at a
# real size cost, -strong covers arrays, address-taken locals and register
# spills, which is where linear overflows land.
STACK_PROTECTOR = -fstack-protector-strong -mstack-protector-guard=global

# -std=gnu2x, NOT gnu23. They select the SAME language mode -- gcc maps c2x/
# gnu2x onto c23/gnu23 -- so this spelling costs no feature, no diagnostic and
# no codegen difference. What it buys is gcc 13, which rejects `gnu23` outright
# ("did you mean -std=gnu2x?") and so cannot build this tree at all. gcc 13 is
# what Ubuntu 24.04 LTS ships, which is both the GitHub runner image and the
# machine that builds our own downstream consumer. Verified: the full tree
# compiles clean on gcc 13 under gnu2x, both arches.
#
# This has now flipped twice (3cfd6be4 -> c5a9127f -> eaa188de) and the second
# flip broke CI for ~2 weeks unnoticed, because CI is dispatch-only. Before
# "modernising" the spelling a third time, check what `gcc -dumpversion` says
# on ubuntu-latest -- the bare-metal C++ toolchains (14.3, both arches) accept
# either spelling and do NOT constrain this choice.
#
# -DAXL_ALLOW_UEFI: this tree IS the backend -- src/, tools/ and the tests all
# legitimately speak UEFI, and <uefi/axl-uefi.h> now refuses to compile without
# it so that ordinary applications cannot reach EDK2 by typing an #include.
# axl-cc grants it per image type (drivers yes, apps only with --allow-uefi).
CFLAGS_BASE = -std=gnu2x -DAXL_ALLOW_UEFI \
              -ffreestanding -fshort-wchar \
              -fno-builtin \
              -fno-math-errno -fno-trapping-math \
              -fno-omit-frame-pointer \
              -fpic $(GCC_ARCH) \
              $(STACK_PROTECTOR) \
              -Wall \
              -DAXL_BACKEND_NATIVE

# pe-set-debug also stamps NX_COMPAT in DllCharacteristics (the images
# are W^X-clean) for Secure-Boot / memory-protected firmware; verified
# by `make check-nx-compat` (scripts/check-pe-nx.py).
#
# Both DEBUG and RELEASE emit DWARF debug info — the .efi PE/COFF
# stays slim because objcopy only carries .dbgdir through, and
# pe-set-debug points the debug data directory at the side-by-side
# .debug file. RELEASE-with-debug-info means a #PF in the firmware
# can be addr2line'd against the artifact a user already has,
# without rebuilding.
ifeq ($(BUILD),RELEASE)
  CFLAGS_BUILD = -Os -g -gdwarf -ffunction-sections -fdata-sections -DNDEBUG
else
  CFLAGS_BUILD = -Og -g -gdwarf -DAXL_MEM_DEBUG \
                 -ffunction-sections -fdata-sections
endif

# C++ flag set for libaxl-cxx.a, for any C++ source under src/, and for the
# check-examples / check-cxx-entry gates.  Matches the hard defaults baked into
# axl-cc's C++ path — no exceptions, no RTTI, no thread-safe statics, C++23.
# Unconditional: see the CXX comment above for what guarding it cost.
#
# NO -ffreestanding, and that is task T3 (AXL-Cxx-Design.md §6a-PLAN). It sets
# __STDC_HOSTED__ to 0, which libstdc++ consults in bits/requires_hosted.h to
# refuse <vector>/<string>/<map> -- so it was the whole of what `--hosted`
# turned off, and there is one C++ mode now. C KEEPS it (see CFLAGS_BASE):
# dropping it there would pull a real <stdio.h> with a FILE and locale
# machinery AXL does not implement. One mode PER LANGUAGE, no user-facing flag.
CXXFLAGS_BASE = -std=c++23 \
                -fshort-wchar \
                -fno-builtin \
                -fno-omit-frame-pointer \
                -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                -fpic $(GCC_ARCH) \
                $(STACK_PROTECTOR) \
                -Wall \
                -DAXL_BACKEND_NATIVE
# -MD -MP for the same reason CFLAGS carries them, and it was missing here for
# as long as the C++ layer existed: no .cpp object had ANY header dependency,
# so editing axl-cxx.hpp (or any .hpp) rebuilt nothing and the next run tested
# the previous binary. That is not a slow-build annoyance, it is a wrong-answer
# generator -- it made a sabotage of axl-istream.hpp read as UNDETECTED when
# the code was never recompiled. On CXXFLAGS, not CXXFLAGS_BASE: check-examples
# and check-cxx-entry compile throwaway objects with the BASE set and would
# scatter .d files outside $(BUILDDIR).
CXXFLAGS      = $(CXXFLAGS_BASE) $(CFLAGS_BUILD) -MD -MP

# NO separate C++ include path. There was an INCLUDES_HOSTED that filtered
# -Ideps/lzma out for the five hosted TUs, and it FILTERED NOTHING: -Ideps/lzma
# is not in $(INCLUDES) at all (see the comment on it above) -- it reaches only
# the two lzma TUs, per-rule. The protection that comment described is real and
# comes from somewhere else, so the filter was a no-op that read like a guard.
# Removed rather than carried forward: every C++ TU can reach <string> now, and
# a no-op guard standing in front of a live hazard is worse than none.
#
# include/compat used to be filtered too, for its `typedef void FILE` against
# the real <stdio.h>. It is gone: C now compiles with a bare-metal cross whose
# newlib supplies the genuine headers.

ifeq ($(ARCH),aa64)
  GCC_CRT0 = $(BUILDDIR)/axl-crt0-gcc-aarch64.o
else
  GCC_CRT0 = $(BUILDDIR)/axl-crt0-gcc-x86_64.o
endif
RELOC_OBJ      = $(BUILDDIR)/axl-reloc.o
DEBUG_INFO_OBJ = $(BUILDDIR)/axl-debug-info.o
# Apps (int main): asm CRT0 → _AxlEntry(C CRT0) → main
LINK_CRT0   = $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(CRT0_OBJ)
# Tests/AXL_APP: asm CRT0 → _AxlEntry(from AXL_APP macro)
LINK_CRT0_T = $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ)

# .eh_frame / .gcc_except_table are here on EVERY link and cost a non-exceptions
# image nothing -- measured byte-identical with `cmp`, because --gc-sections has
# already collected them when the linker script keeps nothing. objcopy takes
# EXACT section names and silently drops what it is not given, so omitting them
# would produce an exceptions image whose unwind tables were linked and then
# thrown away: a failure that appears only at the first throw. What must stay
# conditional is the KEEP() in the linker script (elf_*_efi_eh.lds), which is
# what actually costs a C image +16.8%.
OBJCOPY_SECTIONS = -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym \
                   -j .rel -j .rela -j .rela.dyn -j .reloc -j .rodata -j .dbgdir \
                   -j .eh_frame -j .gcc_except_table

# Host tool: patches PE debug data directory after objcopy
PE_SET_DEBUG = $(BUILDDIR)/pe-set-debug

# Link macro for gcc: ld → ELF .so, objcopy → PE/COFF, pe-set-debug → module name
define LINK_EFI_APP
	$(LD_ELF) $(LDFLAGS_EFI) -T $(EFI_LDS) \
	    -o $(2:.efi=.so) $(LINK_CRT0) $(1) $(PREFIX)/lib/libaxl.a
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --output-target=$(PE_TARGET) --subsystem=10 $(2:.efi=.so) $(2)
	$(PE_SET_DEBUG) $(2)
endef

# Like LINK_EFI_APP but links the minimal CRT0 (axl-cc --minimal-runtime):
# skips _axl_init (registry/atexit/signal). Used to test the minimal entry
# point's exit-status return path.
define LINK_EFI_APP_MINIMAL
	$(LD_ELF) $(LDFLAGS_EFI) -T $(EFI_LDS) \
	    -o $(2:.efi=.so) $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(CRT0_MINIMAL_OBJ) $(1) $(PREFIX)/lib/libaxl.a
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --output-target=$(PE_TARGET) --subsystem=10 $(2:.efi=.so) $(2)
	$(PE_SET_DEBUG) $(2)
endef

define LINK_EFI_TEST
	$(LD_ELF) $(LDFLAGS_EFI) -T $(EFI_LDS) \
	    -o $(2:.efi=.so) $(LINK_CRT0_T) $(1) $(PREFIX)/lib/libaxl.a
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --output-target=$(PE_TARGET) --subsystem=10 $(2:.efi=.so) $(2)
	$(PE_SET_DEBUG) $(2)
endef

define LINK_EFI_DRIVER
	$(LD_ELF) $(LDFLAGS_EFI) -T $(EFI_LDS) \
	    --defsym=_AxlEntry=DriverEntry \
	    -o $(2:.efi=.so) $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(1) $(PREFIX)/lib/libaxl.a
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --output-target=$(PE_TARGET) --subsystem=11 $(2:.efi=.so) $(2)
	$(PE_SET_DEBUG) $(2)
endef

# EMBED_BLOB(name, path) — generate a .s with .incbin around `path`,
# assemble it to .o, and expose the .o as $(BLOB_OBJ_<name>). Mirrors
# what `axl-cc --embed PATH=NAME` does internally; eliminates the
# hand-written .S sidecar from the in-tree examples / tools.
#
# Usage:  $(eval $(call EMBED_BLOB,greeting,sdk/examples/embed-asset.txt))
#         link with: $(BLOB_OBJ_greeting)
# C side: AXL_EMBED_DECLARE(greeting) — emits axl_embedded_greeting{,_end}.
#
# Also exposes the intermediate .s as $(BLOB_SRC_<name>) — a `clean-*`
# recipe that reaps a call site's build products should use that
# variable rather than hand-naming `embed-blob-<name>.s`, so the two
# never drift apart if this naming ever changes.
define EMBED_BLOB
BLOB_OBJ_$(1) := $$(BUILDDIR)/embed-blob-$(1).o
BLOB_SRC_$(1) := $$(BUILDDIR)/embed-blob-$(1).s

$$(BUILDDIR)/embed-blob-$(1).s: $(2) | $$(BUILDDIR)
	@: 'Tabs inside the single-quoted args below are load-bearing'
	@: 'assembler indent — leave alone if you reflow this recipe.'
	@printf '%s\n' \
	    '	.section .rodata' \
	    '	.balign 8' \
	    '	.globl axl_embedded_$(1)' \
	    '	.globl axl_embedded_$(1)_end' \
	    'axl_embedded_$(1):' \
	    '	.incbin "$$<"' \
	    'axl_embedded_$(1)_end:' \
	    '' \
	    '	.section .note.GNU-stack, "", %progbits' \
	    > $$@

$$(BUILDDIR)/embed-blob-$(1).o: $$(BUILDDIR)/embed-blob-$(1).s | $$(BUILDDIR)
	$$(CC) $$(CFLAGS_BASE) -c $$< -o $$@
endef

# ===================================================================
# Common configuration
# ===================================================================

CFLAGS     = $(CFLAGS_BASE) $(CFLAGS_BUILD) -MD -MP
# -Ideps/lzma is NOT here, deliberately. It carries a vendored errno.h that
# SHADOWS the toolchain's real one -- harmless while there was no real one, and
# a live trap now that C compiles against newlib (a TU including <errno.h>
# would get a stub with no ERANGE in it). It is added per-rule to the lzma TUs
# that need it, the same way -Ideps/libvterm is, so the shadow reaches only the
# vendored code that expects it.
#
# This is what keeps C++ safe too, and more sharply since T3: every C++ TU can
# now reach <string>, which includes ext/string_conversions.h, which needs
# ERANGE. Adding -Ideps/lzma to this line would break all of them at once.
INCLUDES   = -Iinclude -Isrc/backend \
             -Ideps/libvterm/include

# ===================================================================
# Optional TLS support (AXL_TLS=1)
# ===================================================================

ifdef AXL_TLS
CFLAGS += -DAXL_HAVE_TLS -Ideps/mbedtls/include \
          -DMBEDTLS_CONFIG_FILE='<axl-mbedtls-config.h>' \
          -Isrc/net -Wno-redundant-decls \
          -U_WIN32 -U_WIN64
endif

# ===================================================================
# Library sources (all modules)
# ===================================================================

LIB_SOURCES = \
    src/backend/native/axl-backend-native.c \
    src/backend/native/axl-backend-native-efi1x.c \
    src/backend/native/axl-backend-native-nosh.c \
    src/backend/native/axl-backend-native-event.c \
    src/backend/native/axl-backend-native-mp.c \
    src/mem/axl-mem.c \
    src/mem/axl-intrinsics.c \
    src/mem/axl-arena.c \
    src/format/axl-format.c \
    src/format/axl-dtoa.c \
    src/format/axl-strtod.c \
    src/log/axl-log.c \
    src/log/axl-log-ring.c \
    src/log/axl-log-line.c \
    src/log/axl-log-file.c \
    src/log/axl-log-serial.c \
    src/data/axl-str.c \
    src/data/axl-str-bmh.c \
    src/data/axl-str-base64.c \
    src/data/axl-str-scan.c \
    src/data/axl-str-compat.c \
    src/data/axl-find.c \
    src/data/axl-regex.c \
    src/data/axl-string.c \
    src/data/axl-str-wide.c \
    src/data/axl-hash-table.c \
    src/data/axl-sidecar.c \
    src/data/axl-class-fmt.c \
    src/data/axl-array.c \
    src/data/axl-list.c \
    src/data/axl-slist.c \
    src/data/axl-queue.c \
    src/data/axl-json-parse.c \
    src/data/axl-json-lex.c \
    src/data/axl-json-build.c \
    src/data/axl-json-io.c \
    src/data/axl-json-print.c \
    src/data/axl-xml-writer.c \
    src/data/axl-xml-parse.c \
    src/data/axl-cache.c \
    src/data/axl-page-cache.c \
    src/data/axl-text-buffer.c \
    src/data/axl-rb-tree.c \
    src/data/axl-piece-tree.c \
    src/data/axl-radix-tree.c \
    src/data/axl-ntree.c \
    src/data/axl-tree.c \
    src/data/axl-ring-buf.c \
    src/data/axl-digest.c \
    src/data/axl-digest-md5.c \
    src/data/axl-digest-sha1.c \
    src/data/axl-digest-sha256.c \
    src/data/axl-digest-crc.c \
    src/data/axl-compress.c \
    src/data/axl-compress-lzma.c \
    deps/lzma/LzmaDec.c \
    deps/lzma/LzmaEnc.c \
    deps/lzma/LzFind.c \
    deps/lzma/CpuArch.c \
    src/data/axl-hmac.c \
    src/data/axl-pbkdf2.c \
    src/data/axl-bytes.c \
    src/stream/axl-stream.c \
    src/stream/axl-stream-buf.c \
    src/stream/axl-stream-file.c \
    src/stream/axl-stream-text.c \
    src/stream/axl-compress-stream.c \
    src/fs/axl-fs.c \
    src/fs/axl-file-writer.c \
    src/fs/axl-file-view.c \
    src/fs/axl-file-gen.c \
    src/fs/axl-fs-provider.c \
    src/fs/axl-device-path.c \
    src/util/axl-debug.c \
    src/util/axl-path.c \
    src/util/axl-hexdump.c \
    src/util/axl-time.c \
    src/util/axl-env.c \
    src/util/axl-sys.c \
    src/util/axl-version.c \
    src/util/axl-nvstore.c \
    src/util/axl-var.c \
    src/util/axl-attempt.c \
    src/util/axl-io-port.c \
    src/util/axl-boot.c \
    src/util/axl-image.c \
    src/util/axl-shell.c \
    src/util/axl-console-emit.c \
    src/util/axl-console-input.c \
    src/util/axl-console-term.c \
    src/util/axl-console-device.c \
    src/util/axl-console-tap.c \
    src/util/axl-console-vt.c \
    src/util/axl-console-vt-enc.c \
    src/util/axl-console-tee.c \
    src/util/axl-console-mirror.c \
    src/util/axl-cpu.c \
    src/util/axl-handle-iter.c \
    src/util/axl-mem-phys.c \
    src/util/axl-mem-region.c \
    src/util/axl-watchdog.c \
    src/util/axl-rng.c \
    src/util/axl-rand.c \
    src/util/axl-protocol.c \
    src/util/axl-driver.c \
    src/util/axl-driver-deps.c \
    src/util/axl-driver-info.c \
    src/util/axl-shared-driver.c \
    src/util/axl-diag.c \
    src/util/axl-config.c \
    src/util/axl-config-file.c \
    src/util/axl-subcommand.c \
    src/util/axl-args.c \
    src/util/axl-sort.c \
    src/util/axl-tar.c \
    src/util/axl-console.c \
    src/util/axl-image-verify.c \
    src/util/axl-clipboard.c \
    src/util/axl-shm.c \
    src/smbios/axl-smbios.c \
    src/acpi/axl-acpi.c \
    src/acpi/axl-acpi-mcfg.c \
    src/acpi/axl-acpi-madt.c \
    src/acpi/axl-acpi-fadt.c \
    src/pci/axl-pci.c \
    src/pci/axl-pci-cap.c \
    src/pci/axl-pci-ids.c \
    src/pci/axl-pci-class.c \
    src/usb/axl-usb.c \
    src/usb/axl-usb-class.c \
    src/usb/axl-usb-ids.c \
    src/block/axl-block.c \
    src/nvme/axl-nvme-decode.c \
    src/nvme/axl-nvme.c \
    src/ata/axl-ata-decode.c \
    src/ata/axl-ata.c \
    src/scsi/axl-scsi-decode.c \
    src/scsi/axl-scsi.c \
    src/smart/axl-smart-normalize.c \
    src/smart/axl-smart.c \
    src/serial/axl-serial.c \
    src/fv/axl-fv.c \
    src/fw/axl-fw.c \
    src/tpm/axl-tpm.c \
    src/tpm/axl-tpm-seal.c \
    src/hii/axl-hii.c \
    src/ramdisk/axl-ramdisk.c \
    src/loop/axl-loop.c \
    src/loop/axl-defer.c \
    src/loop/axl-pubsub.c \
    src/service/axl-service.c \
    src/event/axl-event.c \
    src/event/axl-cancellable.c \
    src/event/axl-wait.c \
    src/task/axl-task-pool.c \
    src/task/axl-buf-pool.c \
    src/task/axl-async.c \
    src/9p/axl-9p-codec.c \
    src/9p/axl-9p-client.c \
    src/9p/axl-9p-mount.c \
    src/9p/axl-9p-server.c \
    src/9p/axl-9p-server-ops.c \
    src/9p/axl-9p-server-io-ops.c \
    src/9p/axl-9p-server-ns-ops.c \
    src/9p/axl-9p-server-fid.c \
    src/net/axl-tcp-sync.c \
    src/net/axl-tcp-async.c \
    src/net/axl-net-wait.c \
    src/net/axl-udp.c \
    src/net/axl-net-ping.c \
    src/net/axl-net-sntp.c \
    src/net/axl-net-arp.c \
    src/net/axl-net-linkstats.c \
    src/net/axl-net-resolve.c \
    src/net/axl-net-interfaces.c \
    src/net/axl-net-nic.c \
    src/net/axl-net-addr.c \
    src/net/axl-net-dhcp.c \
    src/net/axl-net-driver-select.c \
    src/net/axl-net-opts.c \
    src/net/axl-http-core.c \
    src/net/axl-http-server.c \
    src/net/axl-http-route.c \
    src/net/axl-http-conn.c \
    src/net/axl-http-request.c \
    src/net/axl-http-dispatch.c \
    src/net/axl-http-response.c \
    src/net/axl-http-upload.c \
    src/net/axl-http-ws.c \
    src/net/axl-http-webdav.c \
    src/net/axl-http-serve-fs.c \
    src/net/axl-http-client.c \
    src/net/axl-http-client-async.c \
    src/net/axl-tls.c \
    src/net/axl-pk-verify.c \
    src/net/axl-jose.c \
    src/net/axl-crypto-rng.c \
    src/net/axl-consttime.c \
    src/net/axl-scram.c \
    src/net/axl-aead.c \
    src/net/axl-cipher.c \
    src/net/axl-ecdh.c \
    src/net/axl-url.c \
    src/net/axl-inet-address.c \
    src/net/axl-socket.c \
    src/net/axl-socket-client.c \
    src/net/axl-websocket.c \
    src/gfx/axl-gfx.c \
    src/gfx/axl-gfx-output.c \
    src/gfx/axl-font.c \
    src/gfx/axl-cursor.c \
    src/gfx/axl-compositor.c \
    src/gfx/axl-gfx-region.c \
    src/gfx/axl-truetype.c \
    src/gfx/axl-pixmap.c \
    src/gfx/axl-gfx-path.c \
    src/gfx/axl-gfx-stroke.c \
    src/gfx/axl-gfx-rasterize.c \
    src/gfx/axl-gfx-gradient.c \
    src/gfx/axl-gfx-effects.c \
    src/gfx/axl-gfx-display-list.c \
    src/gfx/axl-edid.c \
    src/math/axl-math.c \
    src/gfx/fonts/font-edk2-laffstd.c \
    src/gfx/fonts/font-unifont-16.c \
    src/gfx/fonts/font-dejavu-default.c \
    src/gfx/fonts/font-dejavu-mono.c \
    src/input/axl-input.c \
    src/input/axl-input-gesture.c \
    src/input/axl-input-debounce.c \
    src/input/axl-virtual-pointer.c \
    src/smbus/axl-smbus.c \
    src/smbus/axl-smbus-hc.c \
    src/smbus/axl-smbus-i2c.c \
    src/smbus/axl-smbus-piix4.c \
    src/smbus/axl-smbus-format.c \
    src/ipmi/axl-ipmi.c \
    src/ipmi/axl-ipmi-kcs.c \
    src/ipmi/axl-ipmi-ssif.c \
    src/ipmi/axl-ipmi-edkii.c \
    src/ipmi/axl-ipmi-dell.c \
    src/ipmi/axl-ipmi-cmd.c \
    src/ipmi/axl-ipmi-format.c \
    src/spd/axl-spd.c \
    src/spd/axl-spd-ddr4.c \
    src/spd/axl-spd-ddr5.c \
    src/spd/axl-spd-ids.c \
    src/posix/axl-app.c \
    src/runtime/axl-atexit.c \
    src/runtime/axl-cxxabi.c \
    src/runtime/axl-registry.c \
    src/runtime/axl-runtime.c \
    src/runtime/axl-signal.c \
    src/runtime/axl-stack-guard.c \
    src/vterm/axl-vterm.c \
    src/vterm/axl-console-screen.c \
    deps/libvterm/src/vterm.c \
    deps/libvterm/src/parser.c \
    deps/libvterm/src/state.c \
    deps/libvterm/src/pen.c \
    deps/libvterm/src/encoding.c \
    deps/libvterm/src/unicode.c \
    deps/libvterm/src/keyboard.c \
    deps/libvterm/src/mouse.c
# deps/libvterm/src/screen.c is deliberately absent: AXL binds libvterm's
# Layer 2, and Layer 3 maintains a cell grid the consuming widget already owns.
# It is vendored (see deps/libvterm/README.md) but never compiled.

ifdef AXL_TLS
MBEDTLS_SOURCES = \
    deps/mbedtls/library/aes.c \
    deps/mbedtls/library/asn1parse.c \
    deps/mbedtls/library/asn1write.c \
    deps/mbedtls/library/base64.c \
    deps/mbedtls/library/bignum.c \
    deps/mbedtls/library/bignum_core.c \
    deps/mbedtls/library/chacha20.c \
    deps/mbedtls/library/chachapoly.c \
    deps/mbedtls/library/cipher.c \
    deps/mbedtls/library/cipher_wrap.c \
    deps/mbedtls/library/constant_time.c \
    deps/mbedtls/library/ctr_drbg.c \
    deps/mbedtls/library/ecdh.c \
    deps/mbedtls/library/ecdsa.c \
    deps/mbedtls/library/ecp.c \
    deps/mbedtls/library/ecp_curves.c \
    deps/mbedtls/library/entropy.c \
    deps/mbedtls/library/error.c \
    deps/mbedtls/library/gcm.c \
    deps/mbedtls/library/poly1305.c \
    deps/mbedtls/library/hkdf.c \
    deps/mbedtls/library/md.c \
    deps/mbedtls/library/oid.c \
    deps/mbedtls/library/pem.c \
    deps/mbedtls/library/pk.c \
    deps/mbedtls/library/pkparse.c \
    deps/mbedtls/library/pkwrite.c \
    deps/mbedtls/library/pk_ecc.c \
    deps/mbedtls/library/pk_wrap.c \
    deps/mbedtls/library/rsa.c \
    deps/mbedtls/library/rsa_alt_helpers.c \
    deps/mbedtls/library/platform.c \
    deps/mbedtls/library/platform_util.c \
    deps/mbedtls/library/sha256.c \
    deps/mbedtls/library/sha512.c \
    deps/mbedtls/library/ssl_cache.c \
    deps/mbedtls/library/ssl_ciphersuites.c \
    deps/mbedtls/library/ssl_client.c \
    deps/mbedtls/library/ssl_cookie.c \
    deps/mbedtls/library/ssl_msg.c \
    deps/mbedtls/library/ssl_ticket.c \
    deps/mbedtls/library/ssl_tls.c \
    deps/mbedtls/library/ssl_tls12_client.c \
    deps/mbedtls/library/ssl_tls12_server.c \
    deps/mbedtls/library/version.c \
    deps/mbedtls/library/x509.c \
    deps/mbedtls/library/x509_create.c \
    deps/mbedtls/library/x509_crt.c \
    deps/mbedtls/library/x509write.c \
    deps/mbedtls/library/x509write_crt.c

LIB_SOURCES += src/net/axl-mbedtls-platform.c
LIB_SOURCES += $(MBEDTLS_SOURCES)
endif

BUILDDIR   = $(PREFIX)/build
LIB_OBJS   = $(patsubst %.c,$(BUILDDIR)/%.o,$(notdir $(LIB_SOURCES)))

# libaxl-cxx.a contents — built only when AXL_CPP=1.  Pure-C consumers
# never see this archive.  Contents must keep C-linkage symbols out
# (those go in axl-cxxabi.c → libaxl.a).
#
# axl-cxx-libm.cpp, axl-cxx-rehash.cpp, axl-cxx-rbtree.cpp and
# axl-cxx-hash.cpp exist for consumers who USE the standard containers. A C++
# program that touches none of them pulls none of these: an archive member
# only arrives when some symbol in it is still undefined.
#
# rbtree + hash are what let the containers drop libstdc++.a ENTIRELY:
# measured, they were the only two members still being pulled (tree.o and
# hash_bytes.o). See AXL-Cxx-Design.md section 8 -- redistributing the runtime
# library is the one act the GCC Runtime Library Exception does not cover.
LIB_CXX_SOURCES = src/runtime/axl-cxxabi-ops.cpp \
                  src/runtime/axl-cxx-libm.cpp \
                  src/runtime/axl-cxx-rehash.cpp \
                  src/runtime/axl-cxx-rbtree.cpp \
                  src/runtime/axl-cxx-hash.cpp \
                  src/runtime/axl-cxx-string-inst.cpp \
                  src/runtime/axl-cxx-list.cpp
LIB_CXX_OBJS    = $(patsubst %.cpp,$(BUILDDIR)/%.o,$(notdir $(LIB_CXX_SOURCES)))

ifdef AXL_CPP
LIBAXL_CXX_TARGET = $(PREFIX)/lib/libaxl-cxx.a
else
LIBAXL_CXX_TARGET =
endif

# libaxl-cxxrt.a -- the EXCEPTIONS build's glue, and the ALTERNATIVE to
# libaxl-cxx.a rather than a companion to it. See src/cxxrt/axl-cxxrt.c: the
# bare-metal toolchain's libstdc++/libsupc++ already define 51 of the 54
# symbols libaxl-cxx.a exports, so the two cannot appear in one link.
#
# Built with the BARE-METAL compiler, not $(CC): it has to agree with the
# libstdc++ it is glue for -- newlib's headers, its size_t, its _impure_ptr.
# Building it with the host compiler would produce an archive that links and
# then disagrees about types at the ABI boundary.
#
# Gated on that toolchain actually being installed, so a tree without one
# still builds everything else. AXL_CXXRT_CC is resolved from the same
# manifest as every other toolchain path (scripts/axl-toolchains.conf).
ifeq ($(ARCH),aa64)
  # Same ladder as CXX above, ARM_TOOLCHAIN included: resolving these two
  # differently would build libaxl-cxx.a and libaxl-cxxrt.a with DIFFERENT
  # toolchains, which is precisely the ABI-boundary mismatch using the
  # bare-metal compiler is meant to prevent.
  ifdef ARM_TOOLCHAIN
    AXL_CXXRT_CC = $(or $(AXL_AA64_GXX),$(ARM_TOOLCHAIN)/bin/aarch64-none-elf-g++)
  else
    AXL_CXXRT_CC = $(or $(AXL_AA64_GXX),$(AXL_AA64_GXX_DEFAULT))
  endif
else
  # Resolves identically to $(CXX) now that T2 has landed. Kept spelled out
  # rather than written as $(CXX), because the two answer different questions:
  # $(CXX) is whatever compiles AXL's C++, while this archive is glue for a
  # SPECIFIC libstdc++ and is meaningless built by anything else. Collapsing
  # them would make a future $(CXX) override silently retarget the glue.
  AXL_CXXRT_CC = $(or $(AXL_X64_GXX),$(AXL_X64_GXX_DEFAULT))
endif
AXL_CXXRT_CC := $(patsubst %g++,%gcc,$(AXL_CXXRT_CC))

ifneq ($(wildcard $(AXL_CXXRT_CC)),)
LIBAXL_CXXRT_TARGET = $(PREFIX)/lib/libaxl-cxxrt.a
else
LIBAXL_CXXRT_TARGET =
endif

# ===================================================================
# AXL_TLS state-change detection
# ===================================================================
#
# Toggling AXL_TLS between builds changes which .c files end up in
# LIB_SOURCES (mbedtls files are appended only when AXL_TLS=1). Naive
# `make` is unsafe across the toggle for two reasons:
#
#   1. libaxl.a from a previous AXL_TLS=0 run has its mtime AFTER all
#      its .o files, so make sees it as up-to-date and skips the
#      `ar rcs` step — the new mbedtls .o files never get archived.
#      Result: a "TLS-enabled" libaxl.a that's missing every TLS
#      symbol, silently produces non-TLS tool binaries.
#
#   2. Tool .efi files from a previous run can be NEWER than the old
#      libaxl.a. With AXL_TLS=1 added, libaxl.a will be rebuilt
#      mid-make to a later mtime, but make computed dep freshness at
#      startup using OLD libaxl.a mtime — so it concluded the tools
#      were up-to-date and never re-linked them. End state: tool
#      .efis older than the libaxl.a they're "linked against." Real
#      symptom observed 2026-05-04: `fetch.efi` 22:23, libaxl.a 22:25.
#
# Both are fixed by detecting the toggle at parse time (before any
# rule fires) and wiping the artifacts that would be stale. Subsequent
# rules see a clean build tree and rebuild correctly. A make-time
# state change is rare enough that the wipe cost is acceptable.
# Only act on the toggle when the user is actually building something.
# `make clean*` and `make help` shouldn't trip the state-change wipe,
# nor write the state file (we don't want a clean-tools call to alter
# the recorded state and confuse the next real build).
# The pure-lint gates build no libaxl.a and leave no binary that could go stale
# against its ABI, but they run WITHOUT AXL_TLS — so a bare `make <gate>` after
# an AXL_TLS=1 build reads TLS_STATE=off, sees the toggle, and WIPES the TLS
# tree (out/native-<arch>), forcing a full rebuild. Exclude them (like
# clean/help) so a lint neither wipes the tree nor rewrites the recorded state
# to confuse the next real build.
#
# Missing one of these is not a slow rebuild, it is a DATA RACE: verify.sh runs
# its make job CONCURRENTLY with both arch builds, so an unexcluded gate deletes
# $(BUILDDIR)/*.o and libaxl.a out from under them. check-tautology was missing
# and did exactly that — 311 objects and the archive, mid-build — while
# verify.sh's header promised "Nothing collides on disk".
#
# So the list is defined ONCE here and verify.sh reads it back via
# `make -s print-lint-gates`, rather than keeping a second copy that can drift
# from this one. That drift WAS the bug; a comment saying "keep them in sync"
# would only have described it.
#
# check-nx-compat and check-bss-clear are deliberately absent: both take built
# artifacts as prerequisites, so they are not pure lints and cannot run beside a
# build. They stay out of verify.sh's make job for the same reason.
LINT_GATES := check-ascii check-docs check-test-meta check-dogfood \
    check-cxx-entry check-nul check-test-registered check-tautology \
    check-fuzz-link check-examples check-json-dialect check-flag-parity \
    check-dep-tracking check-cb-noexcept check-toolchain-conf check-uefi-scope \
    check-log-levels check-handle-exclusions

print-lint-gates:
	@echo $(LINT_GATES)

# $(CC)'s libc include directory, reported for TOOLS THAT REPLAY THE COMPILE
# DATABASE WITH A DIFFERENT DRIVER -- clang-tidy above all.
#
# bear records commands whose compiler is $(CC), and the libc headers are
# IMPLICIT in that binary: nothing on the command line names
# <toolchain>/x86_64-elf/include. clang-tidy reads such a command, infers the
# target x86_64-elf from the compiler's NAME, and -- knowing no cross libc --
# falls back to the HOST's /usr/include. That is glibc, for a freestanding
# target.
#
# It resolves on a non-multiarch distro (EL/Fedora keep bits/ directly in
# /usr/include) and FAILS on Debian/Ubuntu, where bits/ lives in the multiarch
# subdirectory clang only adds for a linux-gnu target. So the local gate was
# green while CI's ubuntu:26.04 lint job died on
# `features-time64.h: 'bits/wordsize.h' file not found` (run 31672194387) --
# the same local-vs-CI skew class as the doxygen version skew, and worse than
# an outright failure, because every TU that DID analyze was analyzed against
# the wrong libc.
#
# REPORTED, not appended to $(INCLUDES): `-isystem` on this directory would
# move it AHEAD of the gcc-internal headers in the REAL build, changing which
# limits.h / stdint.h / stdatomic.h / tgmath.h 200+ objects compile against
# (those four exist in both directories). The replay reaches the same order
# WITHOUT that risk, because it can also say -nostdlibinc: compiler headers
# first, this directory after, nothing from the host at all. So the consumers
# pass `-nostdlibinc -idirafter $(CC_LIBC_INCLUDE)` and NOT -isystem, which
# would put the libc first and is how the first version of this got it wrong.
#
# Asked of $(CC) rather than assembled from axl-toolchains.conf's
# *_TOOLCHAIN_DIR, so an AXL_X64_GCC / AXL_AA64_GCC override that repoints the
# compiler cannot leave the lint reading a different toolchain's headers.
# Lazy (`=`), so no make invocation on a toolchain-less box pays for the probe.
CC_LIBC_INCLUDE = $(realpath $(lastword \
    $(shell $(CC) -E -Wp,-v -xc /dev/null 2>&1 | sed -n 's|^ \(/.*\)|\1|p')))

.PHONY: print-cc-libc-include
print-cc-libc-include:
	@dir='$(CC_LIBC_INCLUDE)'; \
	if [ -z "$$dir" ] || [ ! -r "$$dir/string.h" ]; then \
	    echo "print-cc-libc-include: cannot resolve $(CC)'s libc include dir" >&2; \
	    echo "  (resolved to '$$dir'; expected a directory holding string.h)" >&2; \
	    echo "  Refusing to report nothing: a lint that silently loses this" >&2; \
	    echo "  analyzes against the HOST libc -- green here, broken on a" >&2; \
	    echo "  multiarch distro, and wrong in both places." >&2; \
	    exit 1; \
	fi; \
	echo "$$dir"

# The C++ counterpart, and the same replay problem one library up. The C++ lint
# pass used to read the HOST libstdc++ by accident: the compile database named
# host `g++`, so clang inferred a linux-gnu target and found /usr/include/c++.
# T2 moved x64 C++ to the bare-metal cross, and the accident stopped working --
# `#include <string>` became "file not found", which is the better failure but
# still a failure. The fix is to name the cross toolchain's C++ directories.
#
# Only the `/c++/` entries. $(CXX)'s search list also carries the gcc-internal
# and libc directories, which the C rules above already handle correctly, and
# passing them here as -isystem would reintroduce exactly the ordering bug
# CC_LIBC_INCLUDE's comment describes.
#
# Three directories, not one, and all three are needed: the generic headers,
# the TARGET subdirectory (<bits/c++config.h> lives there and everything
# includes it), and `backward`. Asked of $(CXX) for the same reason
# CC_LIBC_INCLUDE is asked of $(CC) -- an AXL_X64_GXX / AXL_AA64_GXX override
# must move the lint with it.
CXX_INCLUDE_DIRS = $(realpath $(shell $(CXX) -E -Wp,-v -xc++ /dev/null 2>&1 \
    | sed -n 's|^ \(/.*/c++/.*\)|\1|p'))

.PHONY: print-cxx-include-dirs
print-cxx-include-dirs:
	@dirs='$(CXX_INCLUDE_DIRS)'; \
	found=0; \
	for d in $$dirs; do [ -r "$$d/vector" ] && found=1; done; \
	if [ -z "$$dirs" ] || [ "$$found" != "1" ]; then \
	    echo "print-cxx-include-dirs: cannot resolve $(CXX)'s libstdc++ dirs" >&2; \
	    echo "  (resolved to '$$dirs'; expected one of them to hold <vector>)" >&2; \
	    echo "  Refusing to report nothing: the C++ lint pass would fall back" >&2; \
	    echo "  to the HOST libstdc++, which is a different library from the" >&2; \
	    echo "  one these objects compile against." >&2; \
	    exit 1; \
	fi; \
	echo $$dirs

# `print-%`, not the two names individually: check-dep-tracking's probe
# includes this Makefile and asks it to expand a flag variable, so its
# MAKECMDGOALS is print-CFLAGS etc. Naming only print-prefix and
# print-lint-gates left that GRANDCHILD make outside the exclusion, so it ran
# the build-state block below -- and verify.sh runs the gate job concurrently
# with both arch builds, so a signature mismatch there WIPES $(BUILDDIR)/*.o
# and libaxl.a mid-build. Demonstrated, not theorised. It is the same failure
# check-tautology caused, and the reason MAKE_CHECKS is derived from
# LINT_GATES rather than kept as a second list.
NONCLEAN_GOALS := $(filter-out clean clean-all clean-tools help check-version \
    print-% $(LINT_GATES),\
    $(or $(MAKECMDGOALS),all))

# The recorded state is a SIGNATURE, not just the AXL_TLS toggle, because two
# different things invalidate the objects and only one of them was covered:
#
#   AXL_TLS  changes WHICH sources compile (mbedtls joins LIB_SOURCES).
#   CFLAGS   changes HOW they compile -- and an object does NOT depend on the
#            Makefile, so editing CFLAGS_BASE rebuilds NOTHING. `make` prints
#            the new flags on the one file it happens to recompile and leaves
#            the other 200 objects built the old way.
#   CROSS    changes WHICH LINKER, ar and objcopy consume them. It was left
#            out while binutils were whatever apt supplied and therefore not a
#            variable at all; they are a toolchain INPUT now
#            (AXL_AA64_BINUTILS_PREFIX, x64 to follow), so swapping them
#            silently reused .efi images linked by the other one -- the exact
#            shape of the CC/CXX miss below, one tool along.
#   CC/CXX   changes WHICH COMPILER emits them. AXL_X64_GXX, AXL_AA64_GXX and
#            ARM_TOOLCHAIN all repoint $(CXX), and the bare-metal toolchain
#            compiles against newlib's headers where the host g++ uses
#            glibc's. Left uncovered, `AXL_X64_GXX=... AXL_CPP=1 make` reused
#            host-built objects and a full-suite run "with the new toolchain"
#            silently measured the old one.
#
# The second cost real time and produced four wrong readings while the stack
# protector was being added, including "0 objects instrumented" on a build
# whose command line plainly carried -fstack-protector-strong, and a sabotage
# that looked UNDETECTED because the restored source was never rebuilt. It is
# the same failure mode as the AXL_TLS toggle -- stale objects that make
# believes are current -- so it gets the same treatment rather than a second
# mechanism.
#
# BUILD (DEBUG/RELEASE) is already isolated: it selects its own PREFIX, so the
# two never share objects. ARCH likewise.
#
# Hashed rather than stored whole: the flag string is ~300 characters and the
# point is only whether it CHANGED.
ifneq ($(NONCLEAN_GOALS),)

# The C cross must exist before any of those goals can compile a thing. Say so
# HERE, once, naming the installer -- otherwise the first recipe dies with a
# bare "No such file or directory" on a path the reader has no reason to
# recognise, which is the exact complaint that produced the AXL_CPP guard
# above. Gated on NONCLEAN_GOALS so `make clean`, `make help` and every lint
# gate still work on a box without a toolchain.
ifeq ($(or $(wildcard $(CC)),$(shell command -v $(CC) 2>/dev/null)),)
  # Two messages, because the right remedy depends on which variant asked.
  # Telling a `cross` user to run the installer would be actively wrong: they
  # have just declared they are supplying the toolchain themselves, so the
  # thing they need is the name of the variable they left unset.
  ifeq ($(AXL_TOOLCHAIN),cross)
    $(error AXL_TOOLCHAIN=cross needs AXL_$(TC_ARCH)_GCC set to your own \
      bare-metal C compiler$(if $(CC), (it resolved to "$(CC)"), (it is unset)). \
      Set AXL_$(TC_ARCH)_GXX and AXL_$(TC_ARCH)_BINUTILS_PREFIX too; the \
      axl-toolchains.conf defaults are deliberately not consulted under this \
      variant. It must target bare metal -- a glibc-targeted cross resolves \
      <string.h> to a hosted libc, which include/compat used to paper over and \
      no longer exists; see docs/AXL-Libc-Substrate-Design.md §4.1b)
  else
    $(error C compiler $(CC) not found — run ./scripts/install-toolchain.sh $(ARCH). \
      C is built with the bare-metal cross on both arches so the tree uses no host \
      headers; see docs/AXL-Libc-Substrate-Design.md §4.1b)
  endif
endif

#
# Written with $(file ...) and hashed from disk rather than piped through a
# shell, because the flags are not shell-safe: AXL_TLS=1 adds
# -DMBEDTLS_CONFIG_FILE='<axl-mbedtls-config.h>', whose embedded quotes end
# the argument and leave `<axl-mbedtls-config.h>` as a REDIRECT. The first
# version of this hashed the empty string under AXL_TLS=1 -- a signature that
# is constant is a check that never fires, and it announced itself only
# because e3b0c44298fc is the recognisable SHA-256 of "".
#
# .axl-flags stays on disk on purpose: it is the COMPILER PAIR followed by the
# exact flag set the objects beside it were built with, which is the first
# thing worth reading when a build looks wrong.
#
# $(CC) and $(CXX) lead it, not just the flags. Selecting a different COMPILER
# changes the objects at least as much as changing a flag does. Left out,
# `AXL_X64_GXX=... AXL_CPP=1 make` reused objects the HOST compiler built and a
# full-suite run "with the new toolchain" silently measured the old one: the
# same wrong-reading this signature already exists to prevent for CFLAGS.
#
# It captures the compiler's SPELLING, not its identity -- `CC = $(CROSS)gcc`
# is a bare name, so an in-place host gcc upgrade or a ccache shim appearing on
# PATH hashes identically and rebuilds nothing. That is the cheap 90%: it
# catches a compiler selected by a different NAME, which is how every toolchain
# override in this tree works. Hashing `$(CC) -dumpversion` would close the
# rest at the price of a machine-dependent .axl-flags.
$(shell mkdir -p $(BUILDDIR))
$(file >$(BUILDDIR)/.axl-flags,$(CC) $(CXX) $(AXL_CXXRT_CC) $(CROSS) $(CFLAGS) $(CXXFLAGS) $(INCLUDES))
BUILD_FLAG_SIG := $(shell sha256sum $(BUILDDIR)/.axl-flags | cut -c1-12)
TLS_STATE := tls=$(if $(AXL_TLS),on,off) flags=$(BUILD_FLAG_SIG)
TLS_STATE_FILE := $(BUILDDIR)/.axl-build-state
PREV_TLS_STATE := $(shell cat $(TLS_STATE_FILE) 2>/dev/null)

ifneq ($(TLS_STATE),$(PREV_TLS_STATE))
ifneq ($(PREV_TLS_STATE),)
$(info build state changed: $(PREV_TLS_STATE) -> $(TLS_STATE); wiping .o, libaxl.a, all .efi/.so under $(PREFIX) to avoid stale-archive linkage and stale-flag objects)
# Wipe everything that links against libaxl.a. The earlier targeted
# wipe missed test binaries (which live at $(PREFIX)/AxlTest*.efi —
# root of PREFIX, not tools/) AND example binaries (hello.efi,
# driver.efi, etc., also root of PREFIX). A test-binary built
# against the old AXL_TLS state links OK against the new libaxl.a
# (no undefined symbols — axl_tls_* has stub fallbacks) but its
# struct ABI (fence sizes, debug fill, struct layout for
# TLS-aware structs like AxlHttpServer) mismatches the library's,
# producing baffling failures like "alloc fill 0xDA" tripping on
# freshly-malloced memory. Blanket-wipe everything that could
# reference the libaxl.a ABI; rebuilds are cheap.
$(shell rm -f $(BUILDDIR)/*.o $(PREFIX)/lib/libaxl.a $(PREFIX)/lib/libaxl-cxx.a $(PREFIX)/lib/libaxl-cxxrt.a $(PREFIX)/*.efi $(PREFIX)/*.so $(PREFIX)/tools/*.efi $(PREFIX)/tools/*.so $(PREFIX)/drivers/*.efi $(PREFIX)/drivers/*.so)
endif
$(shell mkdir -p $(BUILDDIR) && echo $(TLS_STATE) > $(TLS_STATE_FILE))
endif
endif

# CRT0 objects (C entry point bridges).
#   native  -- full runtime: registry, atexit, signal notify, default loop.
#   minimal -- opt-out variant for size-constrained or exit-managed apps.
# Both are built unconditionally; axl-cc picks one at link time.
CRT0_OBJ         = $(BUILDDIR)/axl-crt0-native.o
CRT0_MINIMAL_OBJ = $(BUILDDIR)/axl-crt0-minimal.o

# ===================================================================
# Default target
# ===================================================================

.PHONY: all clean clean-all clean-tools print-prefix hello gfx-demo gfx-window pointer-demo pointer-tune-demo cursor-demo frame-anim-demo keytrace input-demo driver smbus-hc-shim binding-driver crashhandler crashtest radix-demo ring-buf-demo event-demo cancellable-demo runtime-demo echo-server tcp-echo-server echo-client echo-server-sync kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server tests tools check-version check-ascii check-cxx-entry check-test-meta check-docs check-dogfood check-tautology check-nx-compat check-bss-clear check-no-avx check-reloc-coverage check-nul check-test-registered check-fuzz-link check-flag-parity check-dep-tracking check-examples check-json-dialect check-log-levels driver-leak-test driver-identity-test driver-parent-leak-test volume-map-test stdio-bridge-reap-test stdio-bridge-liveness-test stdio-bridge-fix stdio-bridge-self stdio-bridge-leak sd-ergo sd-sibling sd-sibling-probe sd-sibling-driver-a sd-sibling-driver-b io-streams cpu-spin-fixture service-demo service-demo-custom svc-startfail svc-embonly embed-asset gfx-present-selftest gfx-avail-probe cursor-selftest exit-status-selftest exit-status-selftest-minimal compositor-selftest compositor-bench cpu-simd-selftest cpu-topology-selftest task-pool-mp-selftest time-settime-selftest http-plain-selftest gfx-simd-selftest console-text-mode-selftest console-reshape-selftest console-device-smoke console-device-restore-smoke console-device-wide-smoke console-device-input-smoke console-device-input-restore-smoke console-device-wide-restore-smoke console-device-cycle-smoke fs-path-selftest fs-read kbprobe axbench kbtune-drv kbtune-drv-test fbcon pin-svc image-path-test shell-launcher 9p 9p-mount-selftest 9p-server-selftest flushfail-fs-driver console-device-passthrough-smoke cxx-streams-selftest

# Pin the default goal so rule order can't turn check-version (or
# any future helper target) into the default by accident.
.DEFAULT_GOAL := all

all: check-version $(PREFIX)/lib/libaxl.a $(LIBAXL_CXX_TARGET) $(LIBAXL_CXXRT_TARGET) $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(CRT0_OBJ) $(CRT0_MINIMAL_OBJ) $(PE_SET_DEBUG)
	@echo ""
	@echo "  AXL library built (gcc, $(ARCH))"
	@echo "  Library:  $(PREFIX)/lib/libaxl.a"
ifdef AXL_CPP
	@echo "  C++ lib:  $(PREFIX)/lib/libaxl-cxx.a"
endif
	@echo "  Headers:  include/axl.h"
	@echo ""

# Verify VERSION file and include/axl/axl-version.h agree. Use
# scripts/bump-version.sh to update both atomically.
check-ascii:
	@python3 scripts/check-output-ascii.py

# A C++ translation unit's AXL_APP / AXL_DRIVER must emit UNMANGLED firmware
# entry points (_AxlEntry / DriverEntry) — the driver link resolves them by
# exact name (--defsym=_AxlEntry=DriverEntry), so a mangled symbol is an
# undefined-reference link failure. C never triggers this (no name mangling)
# and no C++ driver exists in the tree, so this compile+nm check is the only
# thing guarding the AXL_ENTRY_LINKAGE `extern "C"` wrap. Host g++, .o only.
check-cxx-entry:
	@obj=$$(mktemp --suffix=.o); \
	$(CXX) $(CXXFLAGS_BASE) -Iinclude -c test/cxx-entry-linkage.cpp -o $$obj || \
	  { echo "check-cxx-entry: FAIL — C++ fixture did not compile"; rm -f $$obj; exit 1; }; \
	fail=0; \
	for sym in _AxlEntry DriverEntry; do \
	  if nm $$obj | grep -qE " T $$sym$$"; then \
	    echo "check-cxx-entry: $$sym is unmangled (C linkage)"; \
	  else \
	    echo "check-cxx-entry: FAIL — $$sym is missing or name-mangled in C++ (needs AXL_ENTRY_LINKAGE)"; \
	    nm $$obj | grep -i "$$sym" | sed 's/^/    /'; fail=1; \
	  fi; \
	done; \
	rm -f $$obj; \
	[ $$fail -eq 0 ] && echo "check-cxx-entry: clean"; \
	exit $$fail

# Every QEMU integration test must carry a `# test-meta:` header so it is
# discovered + sharded by run-integration.sh (a new test can't silently escape
# the suite). Run in CI's lint job alongside check-ascii.
check-test-meta:
	@bash test/integration/lib/discover.sh --lint

# No assertion may be spelled `test_check(true, ...)`. It reports PASS while
# evaluating nothing, so it inflates the count with a result nobody produced
# and, worse, reads as coverage. There were 210 of them; every one was either
# padding to keep the pass-count ratchet stable across QEMU images, a
# "does not crash" probe, or a claim the enclosing `if` had already
# established. Each has a proper spelling now:
#
#   padding for an absent device -> test_skip_n(n, why)   (declares the count)
#   "did not fault / no-op"      -> test_survived(name)
#   gated out by the build       -> test_skip(name)       (separate baseline)
#   a claim about a value        -> assert the value
#
# A grep, because the compiler cannot see the difference and the suite going
# green says nothing about an assertion that cannot fail.
check-tautology:
	@: 'Match anywhere on the line, then drop comment lines. Anchoring to line'
	@: 'start instead was tried and let `foo(); test_check(true, ...)` through --'
	@: 'verified by sabotage. The exclusion is what spares the primitives'\'''
	@: 'own docstrings, which have to quote the banned form to explain it.'
	@hits=$$(grep -rn 'test_check(true' test/unit/ \
	         | grep -vE ':[[:space:]]*(\*|//|/\*)' || true); \
	if [ -n "$$hits" ]; then \
	  echo "check-tautology: FAIL — assertions that evaluate nothing:"; \
	  echo "$$hits" | sed 's/^/    /'; \
	  echo "  Use test_skip_n(n, why) / test_survived(name) / test_skip(name),"; \
	  echo "  or assert the value. See test/unit/axl-test.h."; \
	  exit 1; \
	fi; \
	echo "check-tautology: clean — no test_check(true, ...) in the unit suite."

# Verify every public header is wired into the Sphinx reference (catches a
# new header landing without docs). Prose staleness is a workflow concern.
check-docs:
	@python3 scripts/check-doc-coverage.py

# Verify library code routes UEFI protocol / boot-service calls through the
# backend + axl_efi_call macro (the seam that keeps every UEFI touchpoint
# enumerable and swappable), not by calling a protocol method pointer directly.
# A per-file ratchet: fails only on a NEW raw call; existing debt is tracked in
# the script's BASELINE and burned down opportunistically. See
# scripts/check-dogfood.py.
check-dogfood:
	@python3 scripts/check-dogfood.py

# Verify no tracked TEXT file carries a literal NUL byte. Writing a
# backslash-u-0000 / backslash-x-00 escape through an editing tool can insert
# the BYTE instead of the source text, turning a .c or .md binary: git shows no
# diff, grep skips it, review sees nothing. Extension-denylist based (see
# scripts/check-nul.py); ~30 ms over the whole index, so it is cheap enough to
# sit in front of every build.
check-nul:
	@python3 scripts/check-nul.py

# Verify every object compiled from C or C++ is built with -MD -MP. Without
# them the object has NO header dependency, so editing a header rebuilds
# nothing and the next run tests the PREVIOUS binary -- a wrong-answer
# generator, not a slow-build annoyance. CXXFLAGS was missing them for the
# entire life of the C++ layer, and it made a sabotage of axl-istream.hpp
# report as UNDETECTED because the code was never recompiled.
#
# The .axl-build-state signature does NOT cover this: it hashes which FLAGS
# an object was built with, not which HEADERS it depends on.
check-dep-tracking:
	@python3 scripts/check-dep-tracking.py

# check-cb-noexcept -- two halves, and both are load-bearing.
#
# The COMPILE half proves AXL_CB_NOEXCEPT actually rejects a throwing
# callback. A fixture that only checked the accepting case would pass
# identically for a header where the macro expands to nothing -- and it is
# empty in C by design, so a stray #undef, a missing <axl/axl-macros.h> or
# a C++ std below 17 all quietly turn the contract back into a comment.
#
# The STRUCTURAL half proves every callback declaration carries it. The
# fixture can only speak for the two declarations it names, so without
# this a NEW callback added without the macro sails through a green gate.
.PHONY: check-cb-noexcept
check-cb-noexcept:
	@obj=$$(mktemp --suffix=.o); \
	$(CXX) $(CXXFLAGS_BASE) -Iinclude \
	    -c test/cb-noexcept-fixture.cpp -o $$obj 2>/dev/null || \
	  { echo "check-cb-noexcept: FAIL -- a noexcept callback was REJECTED"; \
	    $(CXX) $(CXXFLAGS_BASE) -Iinclude \
	        -c test/cb-noexcept-fixture.cpp -o $$obj 2>&1 | head -5 | sed 's/^/    /'; \
	    rm -f $$obj; exit 1; }; \
	if $(CXX) $(CXXFLAGS_BASE) -Iinclude -DEXPECT_REJECT \
	    -c test/cb-noexcept-fixture.cpp -o $$obj 2>/dev/null; then \
	  echo "check-cb-noexcept: FAIL -- a THROWING callback was ACCEPTED;"; \
	  echo "    AXL_CB_NOEXCEPT is not reaching the typedefs (missing"; \
	  echo "    <axl/axl-macros.h>, or the C++ std is below C++17)"; \
	  rm -f $$obj; exit 1; \
	fi; \
	rm -f $$obj; \
	python3 scripts/check-cb-noexcept.py include/axl

# check-handle-exclusions -- axl::unique_handle is opt-in PER TYPE, and the
# types left out are left out because owning them is a bug.
#
# The mechanism is that the deleter resolves through axl::handle_traits<T>,
# which exists only where a header invoked AXL_DEFINE_AUTOPTR_CLEANUP. So the
# exclusion needs no separate enforcement -- it is the default, and a type
# earns a handle by being bound. What needs guarding is that this stays true
# in BOTH directions, which is why the fixture is compiled three ways.
#
# The ACCEPTING build is not a formality. Without it the gate passes just as
# well for a header where handle_traits was never specialized for anything:
# every type would "correctly" reject, and the feature would be absent rather
# than working.
#
# The REJECTING builds match the static_assert TEXT, not merely the exit
# status. A fixture typo fails to compile exactly like a working exclusion
# does, so an exit-status-only assertion would be green while measuring
# nothing -- and the message is the deliverable here anyway: the consumer who
# reaches for the wrong type gets told the ownership rule at the point of the
# mistake instead of hunting for a header that was never missing.
.PHONY: check-handle-exclusions
check-handle-exclusions:
	@obj=$$(mktemp --suffix=.o); \
	$(CXX) $(CXXFLAGS_BASE) -Iinclude \
	    -c test/handle-exclusion-fixture.cpp -o $$obj 2>/dev/null || \
	  { echo "check-handle-exclusions: FAIL -- a BOUND type was REJECTED;"; \
	    echo "    axl::unique_handle must compile for every type whose header"; \
	    echo "    invokes AXL_DEFINE_AUTOPTR_CLEANUP (or _ARG)."; \
	    $(CXX) $(CXXFLAGS_BASE) -Iinclude \
	        -c test/handle-exclusion-fixture.cpp -o $$obj 2>&1 | head -8 | sed 's/^/    /'; \
	    rm -f $$obj; exit 1; }; \
	for spec in "SURFACE:owned by the compositor surface tree" \
	            "JSON:value type, not a handle"; do \
	    name=$${spec%%:*}; want=$${spec#*:}; \
	    err=$$($(CXX) $(CXXFLAGS_BASE) -Iinclude -DEXPECT_REJECT_$$name \
	        -c test/handle-exclusion-fixture.cpp -o $$obj 2>&1); \
	    if [ $$? -eq 0 ]; then \
	      echo "check-handle-exclusions: FAIL -- EXPECT_REJECT_$$name COMPILED;"; \
	      echo "    that type must not be ownable by axl::unique_handle."; \
	      rm -f $$obj; exit 1; \
	    fi; \
	    case "$$err" in \
	      *"$$want"*) ;; \
	      *) echo "check-handle-exclusions: FAIL -- EXPECT_REJECT_$$name failed,"; \
	         echo "    but not with its poison message. Expected to find:"; \
	         echo "        $$want"; \
	         echo "    A fixture typo fails identically; the message IS the test."; \
	         echo "$$err" | head -8 | sed 's/^/    /'; \
	         rm -f $$obj; exit 1 ;; \
	    esac; \
	done; \
	rm -f $$obj

# Verify every `test_*` function defined in test/unit/*.c is actually reached.
# An unregistered test compiles, prints nothing, and cannot move the pass-count
# ratchet -- it reads as coverage while providing none, and nothing else in the
# build can see it (it is static, so -Wunused-function fires only sometimes).
# Counts calls, function-pointer table entries and AXL_APP as registration;
# comments, string labels and forward declarations do NOT count.
check-test-registered:
	@python3 scripts/check-test-registered.py

# Verify every JSON parse in src/ and tools/ names AXL_JSON_STRICT unless it is
# explicitly marked as reading a LOCAL file. Design decision 40: anything
# crossing the network is strict RFC 8259 in both directions, because a peer
# that sends JSON5 is either broken or probing. That rule was written in this
# tree and, until now, enforced only in a consumer -- SoftBMC built a source
# lint for it after a mutation build showed a JSON5 `PUT /api/users/admin`
# demoting the administrator account. See scripts/check-json-dialect.py.
check-json-dialect:
	@python3 scripts/check-json-dialect.py

# axl_info in library code must justify itself with a /* log-level: */ marker.
#
# A library announcing "transport ready" / "listening" / "network ready" tells
# a caller what its own return value already said, on a run where nothing is
# wrong. This shipped twice: v3.2.1 demoted the eight sites a consumer had
# OBSERVED, the consumer removed its workaround, re-measured, and still saw six
# lines -- because the list came from output rather than from a census. A rule
# that lives only in a document is re-broken by the next author to not read it.
# See scripts/check-log-levels.py and docs/AXL-Coding-Style.md.
check-log-levels:
	@python3 scripts/check-log-levels.py

# Verify every sdk/examples/* source still COMPILES against the current public
# headers.
#
# 20 of the 51 examples were reachable by no build rule and no test at all --
# they are copied verbatim into the .deb / .rpm by scripts/build-packages.sh,
# so the first person to compile one was a consumer. That is not hypothetical:
# commit 1b88001a renamed AxlFsProviderInfo to AxlFsEntry across the tree and
# left sdk/examples/memfs.c behind, and it stayed broken because nothing ever
# fed it to a compiler.
#
# Compile-only, so this is about API DRIFT and nothing else -- the examples the
# Makefile and test/integration/test-axl-cc-*.sh already build keep covering
# the link and the runtime. Objects go to a private prefix rather than
# $(BUILDDIR), so the gate cannot collide with the concurrent arch builds
# verify.sh runs beside it.
# CHECK_EX_SRCDIR is overridable so the "found nothing" arm below is reachable
# from a test rather than merely asserted:
#   make check-examples CHECK_EX_SRCDIR=/tmp/empty   # must FAIL
# The `axl-example: hosted` marker is GONE with T3. It selected a second flag
# set for a .cpp example that includes a libstdc++ header; there is one C++
# mode now, so every example gets the same flags a consumer's `axl-c++` uses
# and the marker would select nothing. Its absence is not a loosening: the
# examples that carried it are still compiled, by the same rule as the rest.
CHECK_EX_SRCDIR = sdk/examples
CHECK_EX_DIR    = out/check-examples
CHECK_EX_MIN    = 20

# Examples compile as a CONSUMER would: without -DAXL_ALLOW_UEFI. Reusing
# CFLAGS_BASE meant the gate granted itself raw-UEFI access the SDK denies an
# application, so an example that no consumer could build still reported
# "compiles". A file that legitimately needs it says so in its own text, the
# same way `axl-example: hosted` already works.
# The C filter is the live one. CXXFLAGS_BASE is written out independently and
# never carried -DAXL_ALLOW_UEFI, so its filter-out removes nothing today --
# kept so the three stay symmetric if that ever changes, not because it is
# currently protecting anything.
CFLAGS_EXAMPLE          = $(filter-out -DAXL_ALLOW_UEFI,$(CFLAGS_BASE))
CXXFLAGS_EXAMPLE        = $(filter-out -DAXL_ALLOW_UEFI,$(CXXFLAGS_BASE))

check-examples:
	@mkdir -p $(CHECK_EX_DIR); \
	srcs=$$(ls $(CHECK_EX_SRCDIR)/*.c $(CHECK_EX_SRCDIR)/*.cpp 2>/dev/null); \
	n=$$(printf '%s\n' $$srcs | grep -c . || true); \
	if [ $$n -lt $(CHECK_EX_MIN) ]; then \
	  echo "check-examples: FAIL — found $$n source(s) under $(CHECK_EX_SRCDIR),"; \
	  echo "  fewer than the $(CHECK_EX_MIN) expected. A gate that compiles nothing"; \
	  echo "  reports clean forever, so too few is a failure, not a pass."; \
	  exit 1; \
	fi; \
	fail=0; \
	for src in $$srcs; do \
	  inc="$(INCLUDES)"; \
	  case $$src in \
	    *.cpp) cc="$(CXX) $(CXXFLAGS_EXAMPLE)" ;; \
	    *)     cc="$(CC) $(CFLAGS_EXAMPLE)" ;; \
	  esac; \
	  if grep -q 'axl-example: allow-uefi' $$src; then \
	    cc="$$cc -DAXL_ALLOW_UEFI"; \
	  fi; \
	  if ! $$cc $(CFLAGS_BUILD) $$inc -c $$src \
	       -o $(CHECK_EX_DIR)/$$(basename $$src).o 2>$(CHECK_EX_DIR)/err; then \
	    echo "check-examples: FAIL — $$src"; \
	    sed 's/^/    /' $(CHECK_EX_DIR)/err; fail=1; \
	  fi; \
	done; \
	rm -f $(CHECK_EX_DIR)/err; \
	if [ $$fail -ne 0 ]; then \
	  echo "  Examples ship in the distro packages — a consumer compiles these."; \
	  exit 1; \
	fi; \
	echo "check-examples: clean — $$n $(CHECK_EX_SRCDIR) sources compile."

# libFuzzer is clang-only, so this gate does not follow $(CC). Override to
# point at a specific clang: make check-fuzz-link FUZZ_CC=/opt/llvm/bin/clang
FUZZ_CC ?= clang

# Verify the host-side fuzz harnesses still LINK (test/fuzz).
#
# Those harnesses are deliberately outside the default build: libFuzzer and
# AddressSanitizer need a host toolchain the freestanding UEFI build cannot
# use. Nothing else in the tree referenced test/fuzz, though, and that is
# precisely how json_fuzz came to sit un-linkable for months while the JSON
# parser it targets was rewritten underneath it -- so ASan/LSan coverage of
# the JSON read path was silently ZERO through the whole flag redesign, and
# two OOM defects there had to be caught by code review instead.
#
# Linking is the cheap half of "the fuzzers still work", so it runs as a gate;
# a long FUZZING session stays opt-in (see test/fuzz/README.md). The binaries
# exist once built, so the gate also replays each committed seed corpus at
# -runs=0 -- deterministic, no mutation, ~0.1s, and it upgrades the claim from
# "it links" to "it links and the seeds are still clean under ASan".
#
# Always builds from clean: test/fuzz/Makefile lists only .c prerequisites, so
# an incremental build would happily report success on a binary stale against a
# changed header -- which is the exact drift this gate exists to catch. ~3s.
# No -j here: it inherits the caller's jobserver, and forcing -jN inside a
# submake makes GNU make drop the jobserver with a warning on stderr.
#
# -artifact_prefix keeps a reproducer from a FAILING run inside test/fuzz/,
# where .gitignore already covers crash-*/leak-*/oom-* and `make -C test/fuzz
# clean` removes them. libFuzzer otherwise drops it in the CURRENT directory,
# which for a gate run from the repo root means an unignored crash-<sha> file
# sitting where `git add -A` would sweep it into a commit.
#
# Skips when clang is absent so a gcc-only checkout can still run the gate set
# -- but ONLY as a developer convenience. Under CI (or AXL_FUZZ_REQUIRED=1) a
# missing clang is a FAILURE, because a gate that silently cannot see is worse
# than no gate: it would report green forever while enforcing nothing, which is
# the same trap CLAUDE.md calls out for the leak checker.
check-fuzz-link:
	@if command -v $(FUZZ_CC) >/dev/null 2>&1; then \
	    $(MAKE) -s -C test/fuzz clean >/dev/null 2>&1; \
	    t0=$$(date +%s%3N); \
	    if $(MAKE) -s -C test/fuzz CC=$(FUZZ_CC) >/dev/null; then \
	        t1=$$(date +%s%3N); \
	        times=""; \
	        for h in url json ipmi; do \
	            s=$$(date +%s%3N); \
	            if ! test/fuzz/$${h}_fuzz -runs=0 -artifact_prefix=test/fuzz/ \
	                 test/fuzz/$${h}_corpus/ \
	                 >/dev/null 2>test/fuzz/.$${h}.seedlog; then \
	                echo "check-fuzz-link: FAIL — $${h}_fuzz crashed on its own seed corpus."; \
	                head -30 test/fuzz/.$${h}.seedlog; rm -f test/fuzz/.*.seedlog; \
	                exit 1; \
	            fi; \
	            e=$$(date +%s%3N); \
	            n=$$(ls test/fuzz/$${h}_corpus | wc -l); \
	            times="$$times $${h}=$$((e-s))ms/$${n}"; \
	        done; \
	        rm -f test/fuzz/.*.seedlog; \
	        echo "check-fuzz-link: clean — 3 harnesses link; seeds replay [build $$((t1-t0))ms;$$times seeds]."; \
	    else \
	        echo "check-fuzz-link: FAIL — a harness in test/fuzz no longer links."; \
	        $(MAKE) -C test/fuzz CC=$(FUZZ_CC) 2>&1 | tail -25; \
	        exit 1; \
	    fi; \
	elif [ -n "$(AXL_FUZZ_REQUIRED)$${CI:-}" ]; then \
	    echo "check-fuzz-link: FAIL — $(FUZZ_CC) not found, and CI / AXL_FUZZ_REQUIRED"; \
	    echo "  says this gate must actually run. Install clang or set FUZZ_CC."; \
	    exit 1; \
	else \
	    echo "check-fuzz-link: SKIP — $(FUZZ_CC) not found (libFuzzer is clang-only)."; \
	fi

# Verify the PE post-processor stamped NX_COMPAT (the produced images are
# W^X-clean, so they must advertise NX compatibility for Secure-Boot /
# memory-protected firmware). Builds one representative app + driver and checks
# both — the bit is set uniformly by pe-set-debug, so a regression fails here.
check-nx-compat: $(PREFIX)/cpu-topology-selftest.efi $(PREFIX)/SmbusHcShim.efi
	@python3 scripts/check-pe-nx.py \
	    $(PREFIX)/cpu-topology-selftest.efi $(PREFIX)/SmbusHcShim.efi

# Verify the crt0 zeroes .bss itself (the linker emits .bss as a NOBITS PE
# section; the UEFI loader is NOT trusted to zero-fill it — 576fd474 stuck the
# mouse by relying on that). Firmware-independent: disassembles _start and
# checks it loads both clear bounds (_bss / _bss_end). The current ARCH's crt0.
check-bss-clear: $(GCC_CRT0)
	@python3 scripts/check-bss-clear.py $(GCC_CRT0)

# Verify no VEX/EVEX-encoded instruction reached a produced image. UEFI runs
# with CR4.OSXSAVE clear, so an AVX instruction is #UD at runtime -- and the
# fault reads like a pointer bug (CR2 = 0 is the tell that it is not one).
#
# Not a hypothetical: this host's gcc defaults to -march=x86-64-v3, so ANY
# compile that loses $(GCC_ARCH) emits VEX for plain scalar double math, and
# the distro's own libstdc++.a carries 49 VEX instructions in
# hashtable_c++0x.o. The same mechanism produced two separate wrong
# conclusions before it was root-caused.
#
# libaxl.a rather than a hand-picked .efi list: objdump disassembles every
# archive member, so ONE prerequisite covers every library translation unit
# instead of only the ones a chosen image happens to link. The linked image
# adds the crt0 and the link itself. The hosted-C++ path -- the one that links
# a third-party archive built to someone else's baseline -- is covered where
# it is produced, by test-cxx-hosted-qemu.sh.
#
# The .so, not the .efi: objcopy does not carry .symtab into the PE image, and
# without symbols the check cannot tell a dispatched AVX routine
# (blend_row_over_avx2) from an accidental one. Same code, both files.
#
# Deliberately NOT in LINT_GATES, for the same reason as check-nx-compat and
# check-bss-clear above: it takes built artifacts as prerequisites, so it is
# not a pure lint and cannot run beside a build. CI runs it explicitly.
CHECK_NO_AVX_TARGETS = $(PREFIX)/lib/libaxl.a $(PREFIX)/cpu-topology-selftest.so \
    $(LIBAXL_CXX_TARGET)

#
# $(LIBAXL_CXX_TARGET) is EMPTY without AXL_CPP=1, and libaxl-cxx.a holds the
# two objects this gate was written for -- axl-cxx-rehash.o and
# axl-cxx-libm.o, the ones that exist precisely because the distro's
# hashtable_c++0x.o carries AVX. A bare `make check-no-avx` therefore scanned
# everything EXCEPT them and printed "clean", which is the failure mode this
# gate is supposed to prevent in other people's code. Same shape as
# check-fuzz-link: say so loudly when running degraded, and refuse outright
# under CI, where nothing is watching the output.
check-no-avx: $(PREFIX)/lib/libaxl.a $(PREFIX)/cpu-topology-selftest.efi \
              $(LIBAXL_CXX_TARGET)
	@if [ -z "$(LIBAXL_CXX_TARGET)" ]; then \
	    if [ -n "$$CI" ] || [ -n "$$AXL_CPP_REQUIRED" ]; then \
	        echo "check-no-avx: FAIL — built without AXL_CPP=1, so libaxl-cxx.a"; \
	        echo "  was NOT scanned. Its axl-cxx-rehash.o / axl-cxx-libm.o are the"; \
	        echo "  objects this gate exists for. Run: make AXL_CPP=1 check-no-avx"; \
	        exit 1; \
	    fi; \
	    echo "check-no-avx: DEGRADED — no AXL_CPP=1, so libaxl-cxx.a was not"; \
	    echo "  scanned. For full coverage: make AXL_CPP=1 check-no-avx"; \
	fi; \
	python3 scripts/check-no-avx.py $(CHECK_NO_AVX_TARGETS)

# Verify the relocation table the crt0 walks is whole and reaches the image.
#
# axl-reloc.c reads DT_RELA / DT_RELASZ and walks that many bytes -- it cannot
# notice the bytes stopped being relocations partway through. On AArch64 an
# -frtti link produced TWO relocation sections at non-contiguous addresses
# while DT_RELA pointed at the first and DT_RELASZ counted both; the image
# faulted before main with virtual calls working and only type_info broken.
# Also catches the objcopy half: `-j` takes EXACT section names, so renaming
# the output section without updating the -j list drops every relocation.
check-reloc-coverage: $(PREFIX)/cpu-topology-selftest.efi
	@python3 scripts/check-reloc-coverage.py $(PREFIX)/cpu-topology-selftest.efi

# The three build paths (Makefile, axl-cc, the CMake package install.sh
# generates) each carry their own copy of the compile flags and the objcopy -j
# list. A pure lint -- reads files, builds nothing -- so it belongs in
# LINT_GATES and can run beside a build.
check-flag-parity:
	@python3 scripts/check-flag-parity.py

# check-ctors -- global constructors that NOTHING WILL RUN.
#
# AXL walks .init_array only (src/runtime/axl-cxxabi.c). The linker scripts
# also bound the legacy .ctors with __CTOR_LIST__/__CTOR_END__ and nothing
# reads those, so a non-empty .ctors is code the author expects to execute and
# that silently does not.
#
# axl-cc checks this per link, and the generated CMake package inherits that
# by calling axl-cc. THE MAKEFILE DOES NOT -- it runs its own ld+objcopy for
# every test image and tool, which is the third-build-path drift
# check-flag-parity exists over. Rather than a fourth copy of the check, this
# scans what the Makefile PRODUCED: one pass over $(PREFIX), covering every
# image at once.
#
# The reachable cause is a compiler, not a source edit: GCC's x86_64-*-elf
# target defaults to .ctors and AXL's own toolchain escapes it only via
# --enable-initfini-array (14.3.0-axl2), so a stale AXL_X64_GXX/GCC silently
# disables every constructor in every image the Makefile builds.
#
# NOT in LINT_GATES, deliberately, and this is the same reasoning
# check-no-avx and check-reloc-coverage already follow. Those gates run
# CONCURRENTLY with both arch builds under verify.sh, and a gate that globs
# $(PREFIX)/*.so would read images mid-write -- or find none at all on a clean
# tree and hard-fail. So it declares what it scans as prerequisites and runs
# from CI beside its siblings. `make ARCH=aa64 check-ctors` covers the other
# arch, which matters more: aa64's compiler is ARM's third-party release.
.PHONY: check-ctors
check-ctors: $(PREFIX)/cpu-topology-selftest.efi $(PREFIX)/hello.efi
	@n=0; bad=0; \
	for so in $(PREFIX)/*.so $(PREFIX)/tools/*.so $(PREFIX)/drivers/*.so; do \
	    [ -f "$$so" ] || continue; \
	    if ! syms=$$($(CROSS)nm "$$so" 2>&1); then \
	        echo "check-ctors: FAIL -- $(CROSS)nm could not read $$so."; \
	        echo "  A gate that cannot see reports clean forever."; \
	        echo "  $$syms"; \
	        exit 1; \
	    fi; \
	    lo=$$(printf '%s\n' "$$syms" | awk '$$3 == "__CTOR_LIST__" { print $$1 }'); \
	    hi=$$(printf '%s\n' "$$syms" | awk '$$3 == "__CTOR_END__" { print $$1 }'); \
	    if [ -z "$$lo" ] || [ -z "$$hi" ]; then continue; fi; \
	    n=$$((n+1)); \
	    if [ "$$lo" != "$$hi" ]; then \
	        echo "check-ctors: FAIL -- $$so has constructors in .ctors that"; \
	        echo "  nothing runs (__CTOR_LIST__=$$lo __CTOR_END__=$$hi)."; \
	        bad=$$((bad+1)); \
	    fi; \
	done; \
	if [ "$$bad" -ne 0 ]; then \
	    echo "  Cause: a compiler without --enable-initfini-array. AXL's own"; \
	    echo "  toolchain has it from 14.3.0-axl2; check AXL_$(shell echo $(ARCH) | tr a-z A-Z)_GXX / _GCC."; \
	    exit 1; \
	fi; \
	if [ "$$n" -eq 0 ]; then \
	    echo "check-ctors: no .so images under $(PREFIX) -- run 'make tools tests' first."; \
	    echo "  A gate that scans nothing reports clean forever."; \
	    exit 1; \
	fi; \
	echo "check-ctors: clean -- $$n image(s), none with unwalked .ctors."

# check-toolchain-conf -- the same anti-drift argument as check-flag-parity,
# applied to WHERE the C++ cross compilers live rather than which flags they
# get. The AArch64 path was spelled out in five places (this Makefile,
# scripts/axl-cc, twice in scripts/install.sh -- one of them inside the
# GENERATED axl-config.cmake -- and the installer); adding x64 would have made
# ten. Now all of them read scripts/axl-toolchains.conf, and this asserts both
# that no build-critical file restates a path and that the manifest's own
# repeated spellings agree. Docs are not scanned: prose quoting a path
# describes it rather than depends on it.
.PHONY: check-toolchain-conf
check-toolchain-conf:
	@python3 scripts/check-toolchain-conf.py

# check-uefi-scope -- the contract check-dogfood cannot make. That gate matches
# CALLS (`->PascalCase(`); this one matches TYPES, constants and `#include
# <uefi/...>`, which is how UEFI actually reaches consumer code. `uefi/` ships
# inside include/axl-sdk/ and axl-cc puts that on -isystem, so every consumer
# has always been able to include it and nothing said so.
#
# include/axl/ is held at ZERO with no allowlist -- it is at zero today, so the
# gate starts green and can only catch a regression. tools/ and sdk/examples/
# may use UEFI but must DECLARE it: the point is not to forbid a protocol shim
# from naming the protocol, it is that `rg EFI_ tools` should mean "these ten
# declared it" rather than "who knows".
.PHONY: check-uefi-scope
check-uefi-scope:
	@python3 scripts/check-uefi-scope.py

check-version:
	@file_ver=$$(cat VERSION); \
	header_ver=$$(sed -n 's/^#define AXL_VERSION_STRING  *"\(.*\)".*/\1/p' include/axl/axl-version.h); \
	if [ "$$file_ver" != "$$header_ver" ]; then \
	    echo "ERROR: VERSION ($$file_ver) != axl-version.h ($$header_ver)"; \
	    echo "       run scripts/bump-version.sh X.Y.Z to update both"; \
	    exit 1; \
	fi

# ===================================================================
# Compile rules (per-directory vpath patterns)
# ===================================================================

$(BUILDDIR)/%.o: src/backend/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/backend/native/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/mem/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/format/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/log/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/data/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/stream/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/fs/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/util/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/service/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/loop/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/event/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/task/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/9p/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/net/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/gfx/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/gfx/fonts/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/math/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/input/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/smbios/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/acpi/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/pci/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/usb/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/block/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/nvme/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/ata/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/scsi/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/smart/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/serial/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/fv/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/fw/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/tpm/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/hii/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/ramdisk/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/smbus/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/ipmi/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/spd/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/posix/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/runtime/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# axl-vterm: the second AxlConsoleOps producer, adapting vendored libvterm's
# Layer 2. Standard INCLUDES already carries -Ideps/libvterm/include for <vterm.h>.
$(BUILDDIR)/%.o: src/vterm/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# C++ pattern for src/runtime/*.cpp.  Only reachable when AXL_CPP=1
# (every .cpp source in libaxl-cxx.a lives here).
# ONE rule for every C++ TU. Five of them (axl-cxx-rehash, -rbtree, -hash,
# -string-inst, -list) used to need a second, hosted flag set because each
# includes a libstdc++ header that bits/requires_hosted.h refuses under
# -ffreestanding; T3 removed that flag from the C++ line entirely, so the split
# and its target-specific-variable machinery are gone with it.
#
# $(GCC_ARCH) is NOT droppable here. Dropping it is exactly how a spike picked
# up AVX from this host's -march=x86-64-v3 default and #UD'd under firmware
# that runs with CR4.OSXSAVE clear -- the fault this file exists to prevent.
# `make check-no-avx` covers the result.
#
# Worth keeping from what stood here: the split was ONCE written as several
# target lines sharing one recipe --
#
#     $(BUILDDIR)/a.o: src/runtime/a.cpp | $(BUILDDIR)
#     $(BUILDDIR)/b.o: src/runtime/b.cpp | $(BUILDDIR)
#     	$(CXX) ... -c $< -o $@
#
# which make reads as TWO rules of which only the LAST has a recipe. It looked
# fine because the objects were already up to date; only a build into a
# throwaway prefix, where every TU compiles fresh, exposed it.
ifdef AXL_CPP
$(BUILDDIR)/%.o: src/runtime/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
endif

$(BUILDDIR)/%.o: src/crt0/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/crt0/%.S | $(BUILDDIR)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

$(BUILDDIR)/%.o: deps/lzma/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Ideps/lzma -DZ7_ST -c $< -o $@

# The one AXL source that includes the vendored lzma headers gets the same
# path, target-scoped rather than global -- see the INCLUDES comment.
$(BUILDDIR)/axl-compress-lzma.o: INCLUDES += -Ideps/lzma

# Vendored libvterm (Layer 2 only). -Ideps/libvterm resolves the AXL compat
# shim that vterm_internal.h pulls in; -Ideps/libvterm/src resolves the
# internal headers and the generated .inc tables.
$(BUILDDIR)/%.o: deps/libvterm/src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Ideps/libvterm -Ideps/libvterm/src -c $< -o $@

ifdef AXL_TLS
$(BUILDDIR)/%.o: deps/mbedtls/library/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
endif

# Host tool: pe-set-debug (built with host compiler, not cross-compiler)
$(PE_SET_DEBUG): scripts/pe-set-debug.c | $(BUILDDIR)
	$(HOSTCC) -Wall -O2 -o $@ $<

# Host tool: fwtool (HOSTCC, native — NOT the cross/EFI toolchain).
#
# Reuses the SAME backend-free parser + LZMA sources as the UEFI fwtool.efi,
# so a golden test (test/integration/test-fwtool-host.sh) can prove the C
# parser decodes real OVMF/AAVMF byte-for-byte identically to the reference
# scripts/extract-fv-shell.py. The shim header (-include) maps AXL's leaf
# primitives (alloc/memcpy/...) onto libc; -DAXL_HOSTED selects fwtool.c's
# libc I/O path. -ffunction-sections + --gc-sections drops the gzip/zlib +
# LZMA-encoder paths (and their axl_crc32/CpuArch references) that the
# decode-only extract path never reaches, so no extra symbols need shimming.
# -DZ7_ST matches the UEFI LZMA build (single-thread, no pthread).
FWTOOL_HOST = $(BUILDDIR)/fwtool-host
FWTOOL_HOST_SRCS = tools/fwtool.c src/fw/axl-fw.c src/data/axl-compress.c \
    src/data/axl-compress-lzma.c \
    deps/lzma/LzmaDec.c deps/lzma/LzmaEnc.c deps/lzma/LzFind.c
$(FWTOOL_HOST): $(FWTOOL_HOST_SRCS) tools/fwtool-host-shim.h | $(BUILDDIR)
	$(HOSTCC) -Wall -O2 -DAXL_HOSTED -DZ7_ST \
	    -ffunction-sections -fdata-sections -Wl,--gc-sections \
	    -include tools/fwtool-host-shim.h -Iinclude -Ideps/lzma \
	    -o $@ $(FWTOOL_HOST_SRCS)

.PHONY: fwtool-host
fwtool-host: $(FWTOOL_HOST)

# `ar rcs` inserts-or-replaces members matching by basename — if a
# source file is renamed or removed, its .o stays in the archive
# forever (and a future build picks the stale copy first since `ar`
# preserves insertion order). Delete the archive before each rebuild
# so only the CURRENT $(LIB_OBJS) make it in. This is what the
# "structural header change → make clean" warning in CLAUDE.md was
# papering over.
# Depends on $(PE_SET_DEBUG) (the PE post-processor) as well as the objects:
# every .efi links libaxl.a, so making the archive depend on the tool means a
# pe-set-debug change re-archives the lib and thereby forces every dependent
# .efi to relink and re-run the post-processor (which stamps NX_COMPAT). The
# tool is NOT archived — the recipe lists $(LIB_OBJS) explicitly, not $^.
$(PREFIX)/lib/libaxl.a: $(LIB_OBJS) $(PE_SET_DEBUG) | $(PREFIX)/lib
	@rm -f $@
	$(AR) rcs $@ $(LIB_OBJS)

# libaxl-cxx.a — companion archive for axl-cc's C++ path.  Same
# stale-member-eviction discipline as libaxl.a.  Built only when
# AXL_CPP=1; pure-C builds never reach this rule.
ifdef AXL_CPP
$(PREFIX)/lib/libaxl-cxx.a: $(LIB_CXX_OBJS) | $(PREFIX)/lib
	@rm -f $@
	$(AR) rcs $@ $^
endif

# Compiled with the bare-metal toolchain's C driver and -ffreestanding: this
# is glue, not application code, and it must not pull newlib's own headers for
# the symbols it is REPLACING.
ifneq ($(LIBAXL_CXXRT_TARGET),)
$(BUILDDIR)/axl-cxxrt-%.o: src/cxxrt/axl-cxxrt-%.c | $(BUILDDIR)
	$(AXL_CXXRT_CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# THREE members, each its own object, because archive members are
# all-or-nothing and one of them carries a reference only the exceptions
# linker script can satisfy. Merged, any link taking the allocator bridge
# (every C++ link does -- operator new calls malloc) would also drag
# axl-cxxrt-eh.o's reference to __eh_frame_start, and --no-undefined would
# reject it before --gc-sections could collect the unreferenced function
# holding it. Split, the bridge is usable without opting into the eh
# machinery.
#
# NOTE the archive itself is consumed by nothing: axl-cc names the three
# OBJECTS individually on an -fexceptions link, because axl-cxxrt-alloc.o has
# to OVERRIDE newlib's malloc, and a definition inside an archive does not win
# -- libc.a's malloc.o gets pulled for its other symbols and multiply-defines
# them. Kept as an archive so `make` has one target to name and so the objects
# have a single build rule.
$(PREFIX)/lib/libaxl-cxxrt.a: $(BUILDDIR)/axl-cxxrt-alloc.o \
                              $(BUILDDIR)/axl-cxxrt-eh.o \
                              $(BUILDDIR)/axl-cxxrt-stubs.o | $(PREFIX)/lib
	@rm -f $@
	$(AR) rcs $@ $^
endif

$(BUILDDIR):
	mkdir -p $@

$(PREFIX)/lib:
	mkdir -p $@

# ===================================================================
# Build hello.efi example
# ===================================================================

hello: $(PREFIX)/hello.efi
	@echo "  Built: $(PREFIX)/hello.efi"

$(PREFIX)/hello.efi: $(BUILDDIR)/hello.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/hello.o,$@)

$(BUILDDIR)/hello.o: sdk/examples/hello.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build gfx-demo.efi example
# ===================================================================

gfx-demo: $(PREFIX)/gfx-demo.efi
	@echo "  Built: $(PREFIX)/gfx-demo.efi"

$(PREFIX)/gfx-demo.efi: $(BUILDDIR)/gfx-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-demo.o,$@)

$(BUILDDIR)/gfx-demo.o: sdk/examples/gfx-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build gfx-window.efi example (graphics showcase, holds on a key)
gfx-window: $(PREFIX)/gfx-window.efi
	@echo "  Built: $(PREFIX)/gfx-window.efi"

$(PREFIX)/gfx-window.efi: $(BUILDDIR)/gfx-window.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-window.o,$@)

$(BUILDDIR)/gfx-window.o: sdk/examples/gfx-window.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build pointer-demo.efi example (live mouse cursor, holds on a key)
pointer-demo: $(PREFIX)/pointer-demo.efi
	@echo "  Built: $(PREFIX)/pointer-demo.efi"

$(PREFIX)/pointer-demo.efi: $(BUILDDIR)/pointer-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/pointer-demo.o,$@)

$(BUILDDIR)/pointer-demo.o: sdk/examples/pointer-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build pointer-tune-demo.efi example (live absolute-pointer tuning bench)
pointer-tune-demo: $(PREFIX)/pointer-tune-demo.efi
	@echo "  Built: $(PREFIX)/pointer-tune-demo.efi"

$(PREFIX)/pointer-tune-demo.efi: $(BUILDDIR)/pointer-tune-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/pointer-tune-demo.o,$@)

$(BUILDDIR)/pointer-tune-demo.o: sdk/examples/pointer-tune-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build cursor-demo.efi example (AxlCursor consumer; holds on a key)
cursor-demo: $(PREFIX)/cursor-demo.efi
	@echo "  Built: $(PREFIX)/cursor-demo.efi"

$(PREFIX)/cursor-demo.efi: $(BUILDDIR)/cursor-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cursor-demo.o,$@)

$(BUILDDIR)/cursor-demo.o: sdk/examples/cursor-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build frame-anim-demo.efi — the E7 frame-clock animation recipe (one frame
# callback per window driving a coordinator; AGT's adoption pattern).
frame-anim-demo: $(PREFIX)/frame-anim-demo.efi
	@echo "  Built: $(PREFIX)/frame-anim-demo.efi"

$(PREFIX)/frame-anim-demo.efi: $(BUILDDIR)/frame-anim-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/frame-anim-demo.o,$@)

$(BUILDDIR)/frame-anim-demo.o: sdk/examples/frame-anim-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build keytrace.efi — keyboard inter-arrival diagnostic for tuning the
# key-debounce default (run over a real iDRAC session; serial/console
# output, no graphics). See docs/AXL-Pointer-Cursor-Design.md sec 3.4.
keytrace: $(PREFIX)/keytrace.efi
	@echo "  Built: $(PREFIX)/keytrace.efi"

$(PREFIX)/keytrace.efi: $(BUILDDIR)/keytrace.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/keytrace.o,$@)

$(BUILDDIR)/keytrace.o: sdk/examples/keytrace.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build gfx-present-selftest.efi — GOP present-pipeline round-trip
# test (G17 + G18).  Run under scripts/run-qemu.sh --gpu by
# test/integration/test-gfx-present-qemu.sh; not part of the
# -nographic unit suite (no GOP there).
# ===================================================================

gfx-present-selftest: $(PREFIX)/gfx-present-selftest.efi
	@echo "  Built: $(PREFIX)/gfx-present-selftest.efi"

$(PREFIX)/gfx-present-selftest.efi: $(BUILDDIR)/gfx-present-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-present-selftest.o,$@)

$(BUILDDIR)/gfx-present-selftest.o: test/integration/gfx-present-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# gfx-avail-probe.efi — reports GOP presence for test-no-gpu-qemu.sh.
gfx-avail-probe: $(PREFIX)/gfx-avail-probe.efi
	@echo "  Built: $(PREFIX)/gfx-avail-probe.efi"

$(PREFIX)/gfx-avail-probe.efi: $(BUILDDIR)/gfx-avail-probe.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-avail-probe.o,$@)

$(BUILDDIR)/gfx-avail-probe.o: test/integration/gfx-avail-probe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# gfx-present-bench.efi — quantifies full-screen render cost (build vs present
# vs memcpy floor) to confirm the "logic runs waste CPU rendering" claim.
gfx-present-bench: $(PREFIX)/gfx-present-bench.efi
	@echo "  Built: $(PREFIX)/gfx-present-bench.efi"

$(PREFIX)/gfx-present-bench.efi: $(BUILDDIR)/gfx-present-bench.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-present-bench.o,$@)

$(BUILDDIR)/gfx-present-bench.o: test/integration/gfx-present-bench.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build gfx-mode-selftest.efi — GOP display-mode enumerate/switch
# round-trip.  Run under scripts/run-qemu.sh --gpu by
# test/integration/test-gfx-mode-qemu.sh; not part of the -nographic
# unit suite (no GOP there).
# ===================================================================

gfx-mode-selftest: $(PREFIX)/gfx-mode-selftest.efi
	@echo "  Built: $(PREFIX)/gfx-mode-selftest.efi"

$(PREFIX)/gfx-mode-selftest.efi: $(BUILDDIR)/gfx-mode-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-mode-selftest.o,$@)

$(BUILDDIR)/gfx-mode-selftest.o: test/integration/gfx-mode-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# console-reshape-selftest.efi - a passthrough take-over device must not hide
# the physical console's text modes, and must reshape through them.
# Driven by test/integration/test-console-reshape-qemu.sh.
# ===================================================================

console-reshape-selftest: $(PREFIX)/console-reshape-selftest.efi
	@echo "  Built: $(PREFIX)/console-reshape-selftest.efi"

$(PREFIX)/console-reshape-selftest.efi: $(BUILDDIR)/console-reshape-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/console-reshape-selftest.o,$@)

$(BUILDDIR)/console-reshape-selftest.o: test/integration/console-reshape-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build console-text-mode-selftest.efi — text-console mode enumerate/
# switch round-trip.  Run under scripts/run-qemu.sh --gpu by
# test/integration/test-console-text-mode-qemu.sh (the -nographic unit
# suite has a single-mode serial console, so it can't exercise a switch).
# ===================================================================

console-text-mode-selftest: $(PREFIX)/console-text-mode-selftest.efi
	@echo "  Built: $(PREFIX)/console-text-mode-selftest.efi"

$(PREFIX)/console-text-mode-selftest.efi: $(BUILDDIR)/console-text-mode-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/console-text-mode-selftest.o,$@)

$(BUILDDIR)/console-text-mode-selftest.o: test/integration/console-text-mode-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build fs-path-selftest.efi — file-layer path resolution (fsN:-qualified,
# root-relative, cwd-relative).  Run against BOTH shells by
# test/integration/test-old-shell-qemu.sh: the modern EDK2 shell resolves
# through EFI_SHELL_PROTOCOL, the old EFI 1.x shell through the backend's
# own SHELL_ENVIRONMENT + EFI_FILE_PROTOCOL path.  Parity is the contract.
# ===================================================================

fs-path-selftest: $(PREFIX)/fs-path-selftest.efi
	@echo "  Built: $(PREFIX)/fs-path-selftest.efi"

$(PREFIX)/fs-path-selftest.efi: $(BUILDDIR)/fs-path-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/fs-path-selftest.o,$@)

$(BUILDDIR)/fs-path-selftest.o: test/integration/fs-path-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# cxx-streams-selftest.efi — axl::cout / axl::cin / axl::cerr and axl::string
# (test-cxx-streams-qemu.sh). Built HERE rather than only by axl-c++ because
# scripts/install.sh stages RELEASE, where AXL_MEM_DEBUG is off and there is
# no leak accounting at all — and axl::cin deliberately has no destructor, so
# its buffers are freed by an axl_atexit hook and nothing else. A leak gate
# that cannot see is worse than none. The test ALSO builds this same source
# with the staged axl-c++, which is what proves the consumer toolchain path.
#
# CXXFLAGS_BASE no longer carries -ffreestanding (T3), so this builds exactly
# as a consumer's C++ does. It used to say "freestanding, the configuration the
# stream layer exists for" -- that rationale went with the mode. The layer is
# kept on a different one, settled 2026-08-16 (AXL-Cxx-Design.md 9c): OOM is a
# value, so axl::string sets bad() where std::string halts, and axl::cin reads
# that to report AXL_NO_RESOURCES. Needs AXL_CPP=1 for libaxl-cxx.a (operator
# new/delete and the std:: halt stubs).
cxx-streams-selftest: $(PREFIX)/cxx-streams-selftest.efi
	@echo "  Built: $(PREFIX)/cxx-streams-selftest.efi"

$(PREFIX)/cxx-streams-selftest.efi: $(BUILDDIR)/cxx-streams-selftest.o \
                                    $(LINK_CRT0) $(PREFIX)/lib/libaxl.a \
                                    $(LIBAXL_CXX_TARGET)
	$(LD_ELF) $(LDFLAGS_EFI) -T $(EFI_LDS) \
	    -o $(@:.efi=.so) $(LINK_CRT0) $(BUILDDIR)/cxx-streams-selftest.o \
	    $(PREFIX)/lib/libaxl.a $(LIBAXL_CXX_TARGET) $(PREFIX)/lib/libaxl.a
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --output-target=$(PE_TARGET) --subsystem=10 $(@:.efi=.so) $@
	$(PE_SET_DEBUG) $@

$(BUILDDIR)/cxx-streams-selftest.o: test/integration/cxx-streams-selftest.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# kbprobe.efi — keyboard event-timing probe + the F1/F3/F2 reader for the kbtune
# bounce A/B (test-kbtune-bounce-qemu.sh, driven by run-qemu --holdkey).
kbprobe: $(PREFIX)/kbprobe.efi
	@echo "  Built: $(PREFIX)/kbprobe.efi"

$(PREFIX)/kbprobe.efi: $(BUILDDIR)/kbprobe.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/kbprobe.o,$@)

$(BUILDDIR)/kbprobe.o: test/integration/kbprobe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build bss-probe.efi — measure + validate large-.bss handling (8 MiB
# static array). Used by the .bss-section build experiment to confirm the
# array is carried as an uninitialized PE section (tiny file) yet still
# zero-filled + writable at runtime under the firmware loader.
# ===================================================================

bss-probe: $(PREFIX)/bss-probe.efi
	@echo "  Built: $(PREFIX)/bss-probe.efi"

$(PREFIX)/bss-probe.efi: $(BUILDDIR)/bss-probe.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/bss-probe.o,$@)

$(BUILDDIR)/bss-probe.o: test/integration/bss-probe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build boot-path-selftest.efi — self-relative file access with no shell.
# Staged into the removable-media boot slot so BdsDxe launches it directly
# (no shell at all); the same binary doubles as the shell-case regression
# guard. Driven by test/integration/test-boot-path-qemu.sh.
# ===================================================================

boot-path-selftest: $(PREFIX)/boot-path-selftest.efi
	@echo "  Built: $(PREFIX)/boot-path-selftest.efi"

$(PREFIX)/boot-path-selftest.efi: $(BUILDDIR)/boot-path-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/boot-path-selftest.o,$@)

$(BUILDDIR)/boot-path-selftest.o: test/integration/boot-path-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build cursor-selftest.efi — AxlCursor on-screen compositing test.
# Run under scripts/run-qemu.sh --gpu by
# test/integration/test-cursor-qemu.sh; not part of the -nographic
# unit suite (no GOP there, so on-screen compositing can't be seen).
# ===================================================================

cursor-selftest: $(PREFIX)/cursor-selftest.efi
	@echo "  Built: $(PREFIX)/cursor-selftest.efi"

$(PREFIX)/cursor-selftest.efi: $(BUILDDIR)/cursor-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cursor-selftest.o,$@)

$(BUILDDIR)/cursor-selftest.o: test/integration/cursor-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build exit-status-selftest.efi — arms an exact EFI_STATUS then returns
# nonzero; test-exit-status-qemu.sh asserts the shell's %lasterror%.
exit-status-selftest: $(PREFIX)/exit-status-selftest.efi
	@echo "  Built: $(PREFIX)/exit-status-selftest.efi"

$(PREFIX)/exit-status-selftest.efi: $(BUILDDIR)/exit-status-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/exit-status-selftest.o,$@)

$(BUILDDIR)/exit-status-selftest.o: test/integration/exit-status-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Same source, linked against the MINIMAL CRT0 (--minimal-runtime): the
# thin-launcher case where main returns rather than calling axl_exit, so the
# armed exit status must be honored on the return path.
exit-status-selftest-minimal: $(PREFIX)/exit-status-selftest-minimal.efi
	@echo "  Built: $(PREFIX)/exit-status-selftest-minimal.efi"

$(PREFIX)/exit-status-selftest-minimal.efi: $(BUILDDIR)/exit-status-selftest.o $(CRT0_MINIMAL_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP_MINIMAL,$(BUILDDIR)/exit-status-selftest.o,$@)

# ===================================================================
# Build compositor-selftest.efi — AxlCompositor end-to-end present test.
# Run under scripts/run-qemu.sh --gpu by
# test/integration/test-compositor-qemu.sh (Phase C1).
# ===================================================================

compositor-selftest: $(PREFIX)/compositor-selftest.efi
	@echo "  Built: $(PREFIX)/compositor-selftest.efi"

$(PREFIX)/compositor-selftest.efi: $(BUILDDIR)/compositor-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/compositor-selftest.o,$@)

$(BUILDDIR)/compositor-selftest.o: test/integration/compositor-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build compositor-bench.efi — present-path profile (E6 follow-on, §10).
# Headless: scripts/run-qemu.sh compositor-bench.efi
# ===================================================================

compositor-bench: $(PREFIX)/compositor-bench.efi
	@echo "  Built: $(PREFIX)/compositor-bench.efi"

$(PREFIX)/compositor-bench.efi: $(BUILDDIR)/compositor-bench.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/compositor-bench.o,$@)

$(BUILDDIR)/compositor-bench.o: test/integration/compositor-bench.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build cpu-simd-selftest.efi — AxlCpu feature detection + AVX
# state-enable path, run under a chosen QEMU CPU model by
# test/integration/test-cpu-simd-qemu.sh (CI's qemu64 has no AVX, so
# the CR4/XSETBV path needs an AVX-capable model to exercise).
# ===================================================================

cpu-simd-selftest: $(PREFIX)/cpu-simd-selftest.efi
	@echo "  Built: $(PREFIX)/cpu-simd-selftest.efi"

$(PREFIX)/cpu-simd-selftest.efi: $(BUILDDIR)/cpu-simd-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cpu-simd-selftest.o,$@)

$(BUILDDIR)/cpu-simd-selftest.o: test/integration/cpu-simd-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build cpu-topology-selftest.efi — axl_cpu_topology() against an
# explicit QEMU -smp layout, run by
# test/integration/test-cpu-topology-qemu.sh (the single-vCPU unit
# harness can't exercise multi-processor counts / location / fill).
# ===================================================================

cpu-topology-selftest: $(PREFIX)/cpu-topology-selftest.efi
	@echo "  Built: $(PREFIX)/cpu-topology-selftest.efi"

$(PREFIX)/cpu-topology-selftest.efi: $(BUILDDIR)/cpu-topology-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cpu-topology-selftest.o,$@)

$(BUILDDIR)/cpu-topology-selftest.o: test/integration/cpu-topology-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build task-pool-mp-selftest.efi — multi-core AxlTaskPool race
# regression. Run by test/integration/test-task-pool-mp-qemu.sh under
# QEMU -smp 4 (single-core boots SKIP).
# ===================================================================

task-pool-mp-selftest: $(PREFIX)/task-pool-mp-selftest.efi
	@echo "  Built: $(PREFIX)/task-pool-mp-selftest.efi"

$(PREFIX)/task-pool-mp-selftest.efi: $(BUILDDIR)/task-pool-mp-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/task-pool-mp-selftest.o,$@)

$(BUILDDIR)/task-pool-mp-selftest.o: test/integration/task-pool-mp-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# axbench — real-hardware AP-pool benchmark tool. Measures AxlTaskPool
# (AP offload) vs the BSP across 8 scenarios; writes a report to stdout or
# a file. Ctrl-C aborts cleanly (test-axbench-ctrlc-qemu.sh). It is a
# first-class tool (in TOOL_NAMES → tools/axbench.efi + devkit.conf, built
# by BUILD_TOOL like every other tool); this is a convenience alias to
# build just it. Run: scripts/run-qemu.sh --qemu-arg -smp --qemu-arg N \
#   out/native-<arch>/tools/axbench.efi
# ===================================================================

axbench: $(PREFIX)/tools/axbench.efi
	@echo "  Built: $(PREFIX)/tools/axbench.efi"

# ===================================================================
# Build time-settime-selftest.efi — round-trip the RTC-write API
# (axl_time_set_realtime / axl_time_set_unix) against OVMF's emulated
# RTC, run by test/integration/test-time-qemu.sh (gRT->SetTime mutates
# firmware state, so there is no unit-test seam for it).
# ===================================================================

time-settime-selftest: $(PREFIX)/time-settime-selftest.efi
	@echo "  Built: $(PREFIX)/time-settime-selftest.efi"

$(PREFIX)/time-settime-selftest.efi: $(BUILDDIR)/time-settime-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/time-settime-selftest.o,$@)

$(BUILDDIR)/time-settime-selftest.o: test/integration/time-settime-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build http-plain-selftest.efi — a plain-HTTP-only client that references no
# TLS. With AXL_TLS=1 it must link WITHOUT mbedTLS (test-tls-strippable.sh).
http-plain-selftest: $(PREFIX)/http-plain-selftest.efi
	@echo "  Built: $(PREFIX)/http-plain-selftest.efi"

$(PREFIX)/http-plain-selftest.efi: $(BUILDDIR)/http-plain-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/http-plain-selftest.o,$@)

$(BUILDDIR)/http-plain-selftest.o: test/integration/http-plain-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build gfx-simd-selftest.efi — validate + benchmark the SIMD blur
# kernel (bit-exact vs scalar reference + speedup) under a chosen QEMU
# CPU model.  Driven by test/integration/test-gfx-simd-qemu.sh.
# ===================================================================

gfx-simd-selftest: $(PREFIX)/gfx-simd-selftest.efi
	@echo "  Built: $(PREFIX)/gfx-simd-selftest.efi"

$(PREFIX)/gfx-simd-selftest.efi: $(BUILDDIR)/gfx-simd-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-simd-selftest.o,$@)

$(BUILDDIR)/gfx-simd-selftest.o: test/integration/gfx-simd-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build 9p-mount-selftest.efi — mounts a host 9P share as a UEFI fsN:
# volume (axl_9p_mount) and proves the mount end-to-end: reads the
# server's seeded /hello.txt THROUGH the published volume and asserts
# it byte-matches the raw-client oracle, then writes a new file
# through the mount and confirms it landed by re-reading it over the
# raw client. Run by test/integration/test-9p-qemu.sh.
# ===================================================================

9p-mount-selftest: $(PREFIX)/9p-mount-selftest.efi
	@echo "  Built: $(PREFIX)/9p-mount-selftest.efi"

$(PREFIX)/9p-mount-selftest.efi: $(BUILDDIR)/9p-mount-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/9p-mount-selftest.o,$@)

$(BUILDDIR)/9p-mount-selftest.o: test/integration/9p-mount-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build 9p-server-selftest.efi — the GUEST half of the Axl9pServer
# live-socket harness: seeds a small tree (on a RAM disk where the
# firmware publishes EFI_RAM_DISK_PROTOCOL, on the boot volume
# otherwise — QEMU takes the latter), exports it with
# axl_9p_server_new + _listen, and pumps the loop until a deadline.
# The assertions live on the host side, in
# test/integration/p9-client.py; run by test-9p-server-qemu.sh.
# ===================================================================

9p-server-selftest: $(PREFIX)/9p-server-selftest.efi
	@echo "  Built: $(PREFIX)/9p-server-selftest.efi"

$(PREFIX)/9p-server-selftest.efi: $(BUILDDIR)/9p-server-selftest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/9p-server-selftest.o,$@)

$(BUILDDIR)/9p-server-selftest.o: test/integration/9p-server-selftest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build flushfail-fs-driver.efi -- publishes the flush-failing
# AxlFsProvider fixture and stays RESIDENT (a driver, so the publication
# outlives the load), letting a TOOL be run against it from the same
# shell. The fixture lives in test/unit because the unit tests are its
# main consumer; this is the one integration image that needs it, hence
# the extra -Itest/unit here and nowhere else.
# Run by test/integration/test-flushfail-tools-qemu.sh.
# ===================================================================

flushfail-fs-driver: $(PREFIX)/flushfail-fs-driver.efi
	@echo "  Built: $(PREFIX)/flushfail-fs-driver.efi"

$(PREFIX)/flushfail-fs-driver.efi: $(BUILDDIR)/flushfail-fs-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/flushfail-fs-driver.o,$@)

$(BUILDDIR)/flushfail-fs-driver.o: test/integration/flushfail-fs-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Itest/unit -c $< -o $@

# ===================================================================
# Build input-demo.efi example
# ===================================================================

input-demo: $(PREFIX)/input-demo.efi
	@echo "  Built: $(PREFIX)/input-demo.efi"

$(PREFIX)/input-demo.efi: $(BUILDDIR)/input-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/input-demo.o,$@)

$(BUILDDIR)/input-demo.o: sdk/examples/input-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build driver.efi example (DXE driver — no CRT0)
# ===================================================================

driver: $(PREFIX)/driver.efi
	@echo "  Built: $(PREFIX)/driver.efi"

$(PREFIX)/driver.efi: $(BUILDDIR)/driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/driver.o,$@)

$(BUILDDIR)/driver.o: sdk/examples/driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build http-server-driver.efi — DXE-driver-mode HTTP server,
# integration-test target for axl_loop_attach_driver + the
# fully-async send_response path.
# ===================================================================

http-server-driver: $(PREFIX)/http-server-driver.efi
	@echo "  Built: $(PREFIX)/http-server-driver.efi"

$(PREFIX)/http-server-driver.efi: $(BUILDDIR)/http-server-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/http-server-driver.o,$@)

$(BUILDDIR)/http-server-driver.o: sdk/examples/http-server-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build service_demo.efi + service_demo-dxe.efi — single-source-file
# AxlService demo. The same service-demo.c is compiled twice via the
# AXL_SERVICE(svc) macro: once with -DAXL_SERVICE_BUILD_DRIVER for
# the driver image, once without (with the driver embedded via
# EMBED_BLOB) for the launcher app. Mirrors what `axl-cc --service`
# does for SDK consumers — see scripts/install.sh.
# ===================================================================

service-demo: $(PREFIX)/service_demo.efi $(PREFIX)/service_demo-dxe.efi
	@echo "  Built: $(PREFIX)/service_demo.efi + service_demo-dxe.efi"

# Driver image — same source, compiled with AXL_SERVICE_BUILD_DRIVER.
$(PREFIX)/service_demo-dxe.efi: $(BUILDDIR)/service-demo-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/service-demo-dxe.o,$@)

$(BUILDDIR)/service-demo-dxe.o: sdk/examples/service-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

# Launcher app — same source, no AXL_SERVICE_BUILD_DRIVER, embeds
# the driver via EMBED_BLOB. Embed symbol axl_embedded_service_demo
# matches AXL_EMBED_DECLARE(service_demo) inside the AXL_SERVICE
# macro.
$(eval $(call EMBED_BLOB,service_demo,$(PREFIX)/service_demo-dxe.efi))

$(PREFIX)/service_demo.efi: $(BUILDDIR)/service-demo-app.o $(BLOB_OBJ_service_demo) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/service-demo-app.o $(BLOB_OBJ_service_demo),$@)

$(BUILDDIR)/service-demo-app.o: sdk/examples/service-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# svc_startfail.efi + svc_startfail-dxe.efi — regression fixture for the
# driver start-failure double-free (test-service-startfail-qemu.sh). Same
# dual-compile-and-embed shape as service-demo; the driver's setup returns
# AXL_ERR so the firmware auto-unloads the errored buffer-loaded image.
# ===================================================================
svc-startfail: $(PREFIX)/svc_startfail.efi $(PREFIX)/svc_startfail-dxe.efi
	@echo "  Built: $(PREFIX)/svc_startfail.efi + svc_startfail-dxe.efi"

$(PREFIX)/svc_startfail-dxe.efi: $(BUILDDIR)/svc-startfail-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/svc-startfail-dxe.o,$@)

$(BUILDDIR)/svc-startfail-dxe.o: sdk/examples/svc-startfail.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(eval $(call EMBED_BLOB,svc_startfail,$(PREFIX)/svc_startfail-dxe.efi))

$(PREFIX)/svc_startfail.efi: $(BUILDDIR)/svc-startfail-app.o $(BLOB_OBJ_svc_startfail) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/svc-startfail-app.o $(BLOB_OBJ_svc_startfail),$@)

$(BUILDDIR)/svc-startfail-app.o: sdk/examples/svc-startfail.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# svc_embonly.efi + svc_embonly-dxe.efi + svc_embonly-decoy-dxe.efi —
# fixture for AxlServiceDeploy.embedded_only (test-service-embedded-only-
# qemu.sh). Launcher embeds the REAL driver; the decoy (same service name,
# -DDECOY marker) is staged as the disk-search filename to prove the search
# is skipped. One source, three artifacts.
# ===================================================================
svc-embonly: $(PREFIX)/svc_embonly.efi $(PREFIX)/svc_embonly-dxe.efi $(PREFIX)/svc_embonly-decoy-dxe.efi
	@echo "  Built: svc_embonly.efi + svc_embonly-dxe.efi + svc_embonly-decoy-dxe.efi"

$(PREFIX)/svc_embonly-dxe.efi: $(BUILDDIR)/svc-embonly-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/svc-embonly-dxe.o,$@)

$(BUILDDIR)/svc-embonly-dxe.o: sdk/examples/svc-embonly.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(PREFIX)/svc_embonly-decoy-dxe.efi: $(BUILDDIR)/svc-embonly-decoy-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/svc-embonly-decoy-dxe.o,$@)

$(BUILDDIR)/svc-embonly-decoy-dxe.o: sdk/examples/svc-embonly.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -DDECOY -c $< -o $@

$(eval $(call EMBED_BLOB,svc_embonly,$(PREFIX)/svc_embonly-dxe.efi))

$(PREFIX)/svc_embonly.efi: $(BUILDDIR)/svc-embonly-app.o $(BLOB_OBJ_svc_embonly) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/svc-embonly-app.o $(BLOB_OBJ_svc_embonly),$@)

$(BUILDDIR)/svc-embonly-app.o: sdk/examples/svc-embonly.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build service_demo_custom.efi + service_demo_custom-dxe.efi —
# worked example showing consumer-visible AxlArgs + AxlConfig usage.
# Same dual-compile pattern as service-demo, but main() is written
# by hand (not via AXL_SERVICE) so the consumer can mix the standard
# start/stop/status verbs with custom verbs (here: `config`).
# ===================================================================

service-demo-custom: $(PREFIX)/service_demo_custom.efi $(PREFIX)/service_demo_custom-dxe.efi
	@echo "  Built: $(PREFIX)/service_demo_custom.efi + service_demo_custom-dxe.efi"

$(PREFIX)/service_demo_custom-dxe.efi: $(BUILDDIR)/service-demo-custom-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/service-demo-custom-dxe.o,$@)

$(BUILDDIR)/service-demo-custom-dxe.o: sdk/examples/service-demo-custom.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(eval $(call EMBED_BLOB,service_demo_custom,$(PREFIX)/service_demo_custom-dxe.efi))

$(PREFIX)/service_demo_custom.efi: $(BUILDDIR)/service-demo-custom-app.o $(BLOB_OBJ_service_demo_custom) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/service-demo-custom-app.o $(BLOB_OBJ_service_demo_custom),$@)

$(BUILDDIR)/service-demo-custom-app.o: sdk/examples/service-demo-custom.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build embed-asset.efi — non-driver worked example for
# <axl/axl-embed.h>. Embeds embed-asset.txt via .incbin and prints
# its bytes at runtime. Demonstrates that the embed framework is
# content-agnostic (bytes in, bytes out — driver .efi is just one
# special case).
# ===================================================================

embed-asset: $(PREFIX)/embed-asset.efi
	@echo "  Built: $(PREFIX)/embed-asset.efi"

$(eval $(call EMBED_BLOB,greeting,sdk/examples/embed-asset.txt))

$(PREFIX)/embed-asset.efi: $(BUILDDIR)/embed-asset.o $(BLOB_OBJ_greeting) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/embed-asset.o $(BLOB_OBJ_greeting),$@)

$(BUILDDIR)/embed-asset.o: sdk/examples/embed-asset.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build driver-leak-test.efi — exercises axl_driver_load +
# set_load_options + unload, asserts no leak. Integration target
# for the LoadOptions-leak fix in src/util/axl-driver.c.
# ===================================================================

driver-leak-test: $(PREFIX)/driver-leak-test.efi
	@echo "  Built: $(PREFIX)/driver-leak-test.efi"

$(PREFIX)/driver-leak-test.efi: $(BUILDDIR)/driver-leak-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/driver-leak-test.o,$@)

$(BUILDDIR)/driver-leak-test.o: test/integration/driver-leak-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build image-path-test.efi + image-path-driver.efi — fixture for the
# axl_app_image_path() synthetic-load contract: a buffer-loaded driver has
# no file it was loaded from, so it must report NULL (and still resolve its
# sidecar via the launcher that DID come from a file).
# ===================================================================

image-path-test: $(PREFIX)/image-path-test.efi $(PREFIX)/image-path-driver.efi
	@echo "  Built: image-path-test.efi + image-path-driver.efi"

$(PREFIX)/image-path-driver.efi: $(BUILDDIR)/image-path-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/image-path-driver.o,$@)
$(BUILDDIR)/image-path-driver.o: test/integration/image-path-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(PREFIX)/image-path-test.efi: $(BUILDDIR)/image-path-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/image-path-test.o,$@)
$(BUILDDIR)/image-path-test.o: test/integration/image-path-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build axl-shell-launcher.efi — the test harness stages this as
# \EFI\BOOT\BOOTX64.EFI in place of the Shell. It sibling-loads Shell.efi with
# LoadOptions "-delay 0" so the EDK2 Shell skips its 5 s startup countdown,
# reclaiming ~5 s of Stall per guest boot. Staged by run-qemu.sh / common-test.sh
# via find_shell_launcher (scripts/axl-common.sh).
# ===================================================================

shell-launcher: $(PREFIX)/axl-shell-launcher.efi
	@echo "  Built: $(PREFIX)/axl-shell-launcher.efi"

$(PREFIX)/axl-shell-launcher.efi: $(BUILDDIR)/axl-shell-launcher.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/axl-shell-launcher.o,$@)

$(BUILDDIR)/axl-shell-launcher.o: test/integration/axl-shell-launcher.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build driver-identity-test.efi — buffer-loads driver.efi and asserts
# the loaded image has a non-NULL, renderable device path (so the aa64
# shell's `dh -p` / `dh -v` does not fault). Integration target for the
# embedded-image-identity synthesis in src/util/axl-driver.c.
# ===================================================================

driver-identity-test: $(PREFIX)/driver-identity-test.efi
	@echo "  Built: $(PREFIX)/driver-identity-test.efi"

$(PREFIX)/driver-identity-test.efi: $(BUILDDIR)/driver-identity-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/driver-identity-test.o,$@)

$(BUILDDIR)/driver-identity-test.o: test/integration/driver-identity-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build driver-parent-leak-test.efi — regression test for the
# shared-driver cross-image "orphaned synthesized device path" leak.
# The controller loads + starts the stdio-bridge-fix launcher (which
# buffer-loads stdio-bridge-driver from its embedded blob then exits),
# unloads the driver from a DIFFERENT image, and asserts no orphaned
# LoadedImageDevicePath handle survives. Reuses the stdio-bridge
# fixture, so it must be built alongside.
# ===================================================================

driver-parent-leak-test: $(PREFIX)/driver-parent-leak-test.efi stdio-bridge-fix
	@echo "  Built: $(PREFIX)/driver-parent-leak-test.efi"

$(PREFIX)/driver-parent-leak-test.efi: $(BUILDDIR)/driver-parent-leak-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/driver-parent-leak-test.o,$@)

$(BUILDDIR)/driver-parent-leak-test.o: test/integration/driver-parent-leak-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build stdio-bridge-reap-test.efi — regression test for the
# stdio-bridge dead-instance leak. Runs alongside stdio-bridge-leak.efi
# (two shell-launched leakers) and asserts dead bridges are reaped (at
# install and on axl_shared_driver_unload) rather than accumulating.
# ===================================================================

stdio-bridge-reap-test: $(PREFIX)/stdio-bridge-reap-test.efi stdio-bridge-leak
	@echo "  Built: $(PREFIX)/stdio-bridge-reap-test.efi"

$(PREFIX)/stdio-bridge-reap-test.efi: $(BUILDDIR)/stdio-bridge-reap-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-reap-test.o,$@)

$(BUILDDIR)/stdio-bridge-reap-test.o: test/integration/stdio-bridge-reap-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build stdio-bridge-liveness-test.efi — regression for the handle-reuse
# false-alive: a bridge that fools the old LoadedImage-proto match must be
# rejected by the per-dispatch token gate. Self-contained (no leaker helper).
# ===================================================================

stdio-bridge-liveness-test: $(PREFIX)/stdio-bridge-liveness-test.efi
	@echo "  Built: $(PREFIX)/stdio-bridge-liveness-test.efi"

$(PREFIX)/stdio-bridge-liveness-test.efi: $(BUILDDIR)/stdio-bridge-liveness-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-liveness-test.o,$@)

$(BUILDDIR)/stdio-bridge-liveness-test.o: test/integration/stdio-bridge-liveness-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build stdio-bridge-fix.efi (+ -driver.efi) — acceptance fixture for
# the shared-driver stdio bridge. The driver image reads axl_stdin /
# writes axl_stdout when the launcher dispatches a verb; the launcher
# embeds the driver and locates it (which installs the stdio bridge).
# Same dual-image embed pattern as service-demo.
# ===================================================================

stdio-bridge-fix: $(PREFIX)/stdio-bridge-fix.efi $(PREFIX)/stdio-bridge-driver.efi
	@echo "  Built: $(PREFIX)/stdio-bridge-fix.efi + stdio-bridge-driver.efi"

# Driver image — DXE driver (no CRT0), publishes the fixture vtable.
$(PREFIX)/stdio-bridge-driver.efi: $(BUILDDIR)/stdio-bridge-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/stdio-bridge-driver.o,$@)

$(BUILDDIR)/stdio-bridge-driver.o: test/integration/stdio-bridge-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Launcher app — embeds the driver via EMBED_BLOB. Embed symbol
# axl_embedded_stdio_bridge_fix matches AXL_EMBED_DECLARE(stdio_bridge_fix).
$(eval $(call EMBED_BLOB,stdio_bridge_fix,$(PREFIX)/stdio-bridge-driver.efi))

$(PREFIX)/stdio-bridge-fix.efi: $(BUILDDIR)/stdio-bridge-launcher.o $(BLOB_OBJ_stdio_bridge_fix) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-launcher.o $(BLOB_OBJ_stdio_bridge_fix),$@)

$(BUILDDIR)/stdio-bridge-launcher.o: test/integration/stdio-bridge-launcher.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Self-locating launcher — resolves the resident driver itself (warm
# fast-path) and installs the bridge via the public escape hatch
# axl_shared_driver_install_stdio_bridge. No embed: it relies on the
# locate launcher above making the driver resident first.
stdio-bridge-self: $(PREFIX)/stdio-bridge-self.efi
	@echo "  Built: $(PREFIX)/stdio-bridge-self.efi"

$(PREFIX)/stdio-bridge-self.efi: $(BUILDDIR)/stdio-bridge-self.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-self.o,$@)

$(BUILDDIR)/stdio-bridge-self.o: test/integration/stdio-bridge-self.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Leaker launcher — installs the bridge then gBS->Exit's without CRT0
# cleanup, leaving a STALE bridge (dangling pipe StdIn). Drives the
# warm-path use-after-free regression in test-driver-stdio-qemu.sh.
stdio-bridge-leak: $(PREFIX)/stdio-bridge-leak.efi
	@echo "  Built: $(PREFIX)/stdio-bridge-leak.efi"

$(PREFIX)/stdio-bridge-leak.efi: $(BUILDDIR)/stdio-bridge-leak.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/stdio-bridge-leak.o,$@)

$(BUILDDIR)/stdio-bridge-leak.o: test/integration/stdio-bridge-leak.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# io-streams — I/O-model redirect fixture tool.
io-streams: $(PREFIX)/io-streams.efi
	@echo "  Built: $(PREFIX)/io-streams.efi"

$(PREFIX)/io-streams.efi: $(BUILDDIR)/io-streams.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/io-streams.o,$@)

$(BUILDDIR)/io-streams.o: test/integration/io-streams.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# cpu-spin-fixture — CPU busy-wait detector fixture (positive +
# negative controls for the CPU-spike sampler). See the file header:
# it busy-waits ON PURPOSE.
cpu-spin-fixture: $(PREFIX)/cpu-spin-fixture.efi
	@echo "  Built: $(PREFIX)/cpu-spin-fixture.efi"

$(PREFIX)/cpu-spin-fixture.efi: $(BUILDDIR)/cpu-spin-fixture.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cpu-spin-fixture.o,$@)

$(BUILDDIR)/cpu-spin-fixture.o: test/integration/cpu-spin-fixture.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build sd-ergo-launcher.efi (+ -driver.efi) — end-to-end fixture built
# ENTIRELY from the turnkey AXL_SHARED_DRIVER / AXL_SHARED_DRIVER_LAUNCHER
# macros (Phase 3 ergonomics). Same dual-image embed pattern as
# stdio-bridge-fix, but the driver + launcher sources are just the three
# app-logic functions + one macro invocation each.
# ===================================================================

sd-ergo: $(PREFIX)/sd-ergo-launcher.efi $(PREFIX)/sd-ergo-driver.efi
	@echo "  Built: sd-ergo-launcher.efi + sd-ergo-driver.efi"

$(PREFIX)/sd-ergo-driver.efi: $(BUILDDIR)/sd-ergo-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/sd-ergo-driver.o,$@)
$(BUILDDIR)/sd-ergo-driver.o: test/integration/sd-ergo-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(eval $(call EMBED_BLOB,sd_ergo_driver,$(PREFIX)/sd-ergo-driver.efi))

$(PREFIX)/sd-ergo-launcher.efi: $(BUILDDIR)/sd-ergo-launcher.o $(BLOB_OBJ_sd_ergo_driver) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/sd-ergo-launcher.o $(BLOB_OBJ_sd_ergo_driver),$@)
$(BUILDDIR)/sd-ergo-launcher.o: test/integration/sd-ergo-launcher.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build pin-svc-launcher.efi + pin-svc-driver-{good,shadow}.efi — fixture
# for AxlServiceDeploy.driver_path (load exactly this file: no 4-path
# search, no embedded fallback). The SAME driver source is built twice with
# -DPIN_VARIANT so the harness can tell which copy actually came up; the
# launcher embeds the SHADOW build, and the harness also stages the shadow
# where the default search looks first.
# ===================================================================

pin-svc: $(PREFIX)/pin-svc-launcher.efi $(PREFIX)/pin-svc-driver-good.efi $(PREFIX)/pin-svc-driver-shadow.efi
	@echo "  Built: pin-svc-launcher.efi + pin-svc-driver-{good,shadow}.efi"

$(PREFIX)/pin-svc-driver-good.efi: $(BUILDDIR)/pin-svc-driver-good.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/pin-svc-driver-good.o,$@)
$(BUILDDIR)/pin-svc-driver-good.o: test/integration/pin-svc-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DPIN_VARIANT='"good"' -c $< -o $@

$(PREFIX)/pin-svc-driver-shadow.efi: $(BUILDDIR)/pin-svc-driver-shadow.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/pin-svc-driver-shadow.o,$@)
$(BUILDDIR)/pin-svc-driver-shadow.o: test/integration/pin-svc-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DPIN_VARIANT='"shadow"' -c $< -o $@

$(eval $(call EMBED_BLOB,pin_svc_shadow,$(PREFIX)/pin-svc-driver-shadow.efi))

$(PREFIX)/pin-svc-launcher.efi: $(BUILDDIR)/pin-svc-launcher.o $(BLOB_OBJ_pin_svc_shadow) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/pin-svc-launcher.o $(BLOB_OBJ_pin_svc_shadow),$@)
$(BUILDDIR)/pin-svc-launcher.o: test/integration/pin-svc-launcher.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build sd-sibling-probe.efi + sd-sibling-driver-{a,b}.efi — RED fixture
# for the sibling-locate hard-fail + default-search sibling-first reorder
# (docs/superpowers/specs/2026-07-04-shared-driver-sibling-locate-design.md).
# The probe is a plain shell app (public headers only, no embedded blob);
# the SAME driver source is built twice with -DDRIVER_TAG=A / =B so the
# probe can tell which copy of the driver a locate call resolved.
# ===================================================================

sd-sibling: $(PREFIX)/sd-sibling-probe.efi $(PREFIX)/sd-sibling-driver-a.efi $(PREFIX)/sd-sibling-driver-b.efi
	@echo "  Built: sd-sibling-probe.efi + sd-sibling-driver-a.efi + sd-sibling-driver-b.efi"

sd-sibling-probe: $(PREFIX)/sd-sibling-probe.efi
	@echo "  Built: $(PREFIX)/sd-sibling-probe.efi"

$(PREFIX)/sd-sibling-probe.efi: $(BUILDDIR)/sd-sibling-probe.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/sd-sibling-probe.o,$@)
$(BUILDDIR)/sd-sibling-probe.o: test/integration/sd-sibling-probe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

sd-sibling-driver-a: $(PREFIX)/sd-sibling-driver-a.efi
	@echo "  Built: $(PREFIX)/sd-sibling-driver-a.efi"

$(PREFIX)/sd-sibling-driver-a.efi: $(BUILDDIR)/sd-sibling-driver-a.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/sd-sibling-driver-a.o,$@)
$(BUILDDIR)/sd-sibling-driver-a.o: test/integration/sd-sibling-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DDRIVER_TAG=A -c $< -o $@

sd-sibling-driver-b: $(PREFIX)/sd-sibling-driver-b.efi
	@echo "  Built: $(PREFIX)/sd-sibling-driver-b.efi"

$(PREFIX)/sd-sibling-driver-b.efi: $(BUILDDIR)/sd-sibling-driver-b.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/sd-sibling-driver-b.o,$@)
$(BUILDDIR)/sd-sibling-driver-b.o: test/integration/sd-sibling-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DDRIVER_TAG=B -c $< -o $@

# ===================================================================
# Build fs-read-probe.efi (launcher) + fs-read-driver.efi (resident driver)
# — a resident-driver file-READ fixture. Proves a file stream read via
# axl_fopen -> axl_text_stream_wrap -> axl_readline works the SAME from a
# resident driver (no SHELL_INTERFACE on its LoadedImage) as from a standalone
# app, on the old EFI 1.x shell. The shared read chain (fs-read-common.c) is
# linked into BOTH so the two contexts run byte-identical code.
# ===================================================================

fs-read: $(PREFIX)/fs-read-probe.efi $(PREFIX)/fs-read-driver.efi
	@echo "  Built: fs-read-probe.efi + fs-read-driver.efi"

$(PREFIX)/fs-read-probe.efi: $(BUILDDIR)/fs-read-probe.o $(BUILDDIR)/fs-read-common.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/fs-read-probe.o $(BUILDDIR)/fs-read-common.o,$@)
$(BUILDDIR)/fs-read-probe.o: test/integration/fs-read-probe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(PREFIX)/fs-read-driver.efi: $(BUILDDIR)/fs-read-driver.o $(BUILDDIR)/fs-read-common.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/fs-read-driver.o $(BUILDDIR)/fs-read-common.o,$@)
$(BUILDDIR)/fs-read-driver.o: test/integration/fs-read-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/fs-read-common.o: test/integration/fs-read-common.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build SmbusHcShim.efi — DXE driver that publishes
# EFI_I2C_MASTER_PROTOCOL on QEMU's ICH9 SMBus (for SSIF test-ipmi)
# ===================================================================

smbus-hc-shim: $(PREFIX)/SmbusHcShim.efi
	@echo "  Built: $(PREFIX)/SmbusHcShim.efi"

$(PREFIX)/SmbusHcShim.efi: $(BUILDDIR)/smbus-hc-shim.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/smbus-hc-shim.o,$@)

$(BUILDDIR)/smbus-hc-shim.o: sdk/examples/smbus-hc-shim.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build binding-driver.efi — canonical Type-B (UEFI Driver Model)
# example: AxlDriverBinding driving Supported/Start/Stop
# ===================================================================

binding-driver: $(PREFIX)/binding-driver.efi
	@echo "  Built: $(PREFIX)/binding-driver.efi"

$(PREFIX)/binding-driver.efi: $(BUILDDIR)/binding-driver.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/binding-driver.o,$@)

$(BUILDDIR)/binding-driver.o: sdk/examples/binding-driver.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# CrashHandler — reference DXE driver that captures CPU exceptions to
# NVRAM, + CrashTest, the app that deliberately faults to exercise it.
# The driver is three TUs sharing drivers/crashhandler/crashhandler.h;
# the binary-format contract lives in <axl/axl-crashrecord.h>.
# -fno-omit-frame-pointer (already in CFLAGS_BASE) keeps the FP chain
# the unwinder walks; the default DEBUG build's -g -gdwarf gives
# rsod-decode.py the line info it resolves against.
# ===================================================================
CRASHHANDLER_OBJS = $(BUILDDIR)/crashhandler-entrypoint.o \
                    $(BUILDDIR)/crashhandler-exception.o \
                    $(BUILDDIR)/crashhandler-report.o

crashhandler: $(PREFIX)/drivers/crashhandler.efi
	@echo "  Built: $(PREFIX)/drivers/crashhandler.efi"

$(PREFIX)/drivers/crashhandler.efi: $(CRASHHANDLER_OBJS) $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(CRASHHANDLER_OBJS),$@)

$(BUILDDIR)/crashhandler-%.o: drivers/crashhandler/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Idrivers/crashhandler -c $< -o $@

# console-device-smoke.efi — DEBUG-OVMF smoke consumer driver for
# axl-console-device: installs the take-over device and renders its ops to a GOP
# grid. LOCAL-ONLY (needs a GPU + DEBUG OVMF); see the file header for the run
# recipe and test-console-device-qemu.sh.
console-device-smoke: $(PREFIX)/drivers/console-device-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-smoke.efi"

$(PREFIX)/drivers/console-device-smoke.efi: $(BUILDDIR)/console-device-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-smoke.o,$@)

$(BUILDDIR)/console-device-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# console-device-passthrough-smoke.efi — same source, -DPASSTHROUGH_LOCAL: takes
# over WITHOUT evicting the firmware consoles, so GraphicsConsole keeps painting the
# local display while our grid still receives every op. Drives the passthrough
# scenario of test-console-device-qemu.sh (the inverse of Scenario 1's clean-region
# check: ink past the grid proves the local console is still alive).
console-device-passthrough-smoke: $(PREFIX)/drivers/console-device-passthrough-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-passthrough-smoke.efi"

$(PREFIX)/drivers/console-device-passthrough-smoke.efi: $(BUILDDIR)/console-device-passthrough-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-passthrough-smoke.o,$@)

$(BUILDDIR)/console-device-passthrough-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DPASSTHROUGH_LOCAL -c $< -o $@

# console-device-restore-smoke.efi — same source, -DSELF_UNINSTALL_MS: takes over,
# then after that many ms uninstalls the device so the re-tagged firmware console
# comes back. Drives Scenario 2 of test-console-device-qemu.sh (uninstall-restore).
console-device-restore-smoke: $(PREFIX)/drivers/console-device-restore-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-restore-smoke.efi"

$(PREFIX)/drivers/console-device-restore-smoke.efi: $(BUILDDIR)/console-device-restore-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-restore-smoke.o,$@)

$(BUILDDIR)/console-device-restore-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DSELF_UNINSTALL_MS=4000 -c $< -o $@

# console-device-wide-smoke.efi — same source at a NON-80x25 advertised geometry
# (SMOKE_COLS/SMOKE_ROWS, default 142x44) to exercise a wide take-over console
# against the shell's ConsoleLogger. Built with -DAUTO_ALT=true to match axcon and
# -DPRECACHE_SMALL to make the ConsoleLogger stale-RowsPerScreen bug DETERMINISTIC:
# the driver forces the shell's ConsoleLogger to cache mode 0 (80x25) right before it
# takes over at the taller grid, so absent the install-time SetMode re-sync in
# axl_console_device.c the shell's history bound overflows on scroll every time
# (ConsoleLogger.c:489 CpuDeadLoop). Without the forcing the assert is ~1-in-6
# intermittent -- a poor regression guard. SMOKE_EXTRA appends further -D for ad-hoc
# probing. Drives Scenario 3's geometry regression.
SMOKE_COLS ?= 142
SMOKE_ROWS ?= 44
console-device-wide-smoke: $(PREFIX)/drivers/console-device-wide-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-wide-smoke.efi ($(SMOKE_COLS)x$(SMOKE_ROWS))"

$(PREFIX)/drivers/console-device-wide-smoke.efi: $(BUILDDIR)/console-device-wide-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-wide-smoke.o,$@)

$(BUILDDIR)/console-device-wide-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DGRID_COLS=$(SMOKE_COLS) -DGRID_ROWS=$(SMOKE_ROWS) -DAUTO_ALT=true -DPRECACHE_SMALL $(SMOKE_EXTRA) -c $< -o $@

# console-device-input-smoke.efi — same source with -DTAKE_INPUT: the device also
# becomes the sole ConInEx (evicts the raw keyboard) + runs the read loop, so a
# --sendkey keystroke can ONLY reach the shell through our relay. Drives the input
# scenario of test-console-device-qemu.sh (keys reach the shell -> our grid renders
# the typed command's output; the evicted keyboard proves no double-delivery).
console-device-input-smoke: $(PREFIX)/drivers/console-device-input-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-input-smoke.efi"

$(PREFIX)/drivers/console-device-input-smoke.efi: $(BUILDDIR)/console-device-input-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-input-smoke.o,$@)

$(BUILDDIR)/console-device-input-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DTAKE_INPUT $(SMOKE_EXTRA) -c $< -o $@

# console-device-input-restore-smoke.efi — -DTAKE_INPUT + -DSELF_UNINSTALL_MS: takes
# over BOTH output and input, then self-uninstalls, exercising the ConIn teardown
# (DisconnectController-before-free for our ConInEx + re-admit the keyboard, the input
# mirror of the e051c0db UAF fix) in REAL firmware. A ConIn teardown UAF would wedge
# the uninstall before it drives the restored gST->ConOut, leaving the frame black
# (the --restored check then FAILs). Closes the input-teardown coverage gap.
console-device-input-restore-smoke: $(PREFIX)/drivers/console-device-input-restore-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-input-restore-smoke.efi"

$(PREFIX)/drivers/console-device-input-restore-smoke.efi: $(BUILDDIR)/console-device-input-restore-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-input-restore-smoke.o,$@)

$(BUILDDIR)/console-device-input-restore-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DSELF_UNINSTALL_MS=4000 -DTAKE_INPUT $(SMOKE_EXTRA) -c $< -o $@

# console-device-wide-restore-smoke.efi — NON-80x25 (142x44) + -DSELF_UNINSTALL_MS:
# takes over at a wide geometry, then self-uninstalls. Regression guard for the
# ConSplitter mode-reconstruction assert (ConSplitter.c:2983 CpuDeadLoop under DEBUG
# OVMF): re-adding GraphicsConsole while our single non-80x25 device is still a
# fan-out member makes ConSplitterAddGraphicsOutputMode's SetMode fall through to the
# 80x25 BaseMode and fail. axl_console_device_uninstall disconnects our device from
# the aggregates BEFORE re-adding the firmware console, so the reconstruction sees
# only firmware consoles. The 80x25 restore-smoke could not catch this (80x25 IS the
# BaseMode). Drives the wide-restore scenario of test-console-device-qemu.sh.
console-device-wide-restore-smoke: $(PREFIX)/drivers/console-device-wide-restore-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-wide-restore-smoke.efi ($(SMOKE_COLS)x$(SMOKE_ROWS))"

$(PREFIX)/drivers/console-device-wide-restore-smoke.efi: $(BUILDDIR)/console-device-wide-restore-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-wide-restore-smoke.o,$@)

$(BUILDDIR)/console-device-wide-restore-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DGRID_COLS=$(SMOKE_COLS) -DGRID_ROWS=$(SMOKE_ROWS) -DAUTO_ALT=true -DSELF_UNINSTALL_MS=4000 $(SMOKE_EXTRA) -c $< -o $@

# console-device-cycle-smoke.efi — NON-80x25 (142x44), -DCYCLE_COUNT=3: take over ->
# restore -> re-take-over three times in one boot (each ~1.2s). Guards the multi-cycle
# path a single uninstall can't: a second uninstall, re-eviction of the just-restored
# GraphicsConsole, and any cumulative ConSplitter state (the mode assert would fire on
# every re-add). Leaves the console restored at the end; --restored proves the final
# state is live and no cycle wedged.
console-device-cycle-smoke: $(PREFIX)/drivers/console-device-cycle-smoke.efi
	@echo "  Built: $(PREFIX)/drivers/console-device-cycle-smoke.efi ($(SMOKE_COLS)x$(SMOKE_ROWS), 3 cycles)"

$(PREFIX)/drivers/console-device-cycle-smoke.efi: $(BUILDDIR)/console-device-cycle-smoke.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/drivers
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/console-device-cycle-smoke.o,$@)

$(BUILDDIR)/console-device-cycle-smoke.o: test/integration/console-device-smoke.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DGRID_COLS=$(SMOKE_COLS) -DGRID_ROWS=$(SMOKE_ROWS) -DAUTO_ALT=true -DCYCLE_COUNT=3 -DSELF_UNINSTALL_MS=1200 $(SMOKE_EXTRA) -c $< -o $@

$(PREFIX)/drivers:
	@mkdir -p $@

crashtest: $(PREFIX)/tools/crashtest.efi
	@echo "  Built: $(PREFIX)/tools/crashtest.efi"

$(PREFIX)/tools/crashtest.efi: $(BUILDDIR)/crashtest.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(BUILDDIR)/crashtest.o,$@)

$(BUILDDIR)/crashtest.o: tools/crashtest.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build radix-demo.efi example
# ===================================================================

radix-demo: $(PREFIX)/radix-demo.efi
	@echo "  Built: $(PREFIX)/radix-demo.efi"

$(PREFIX)/radix-demo.efi: $(BUILDDIR)/radix-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/radix-demo.o,$@)

$(BUILDDIR)/radix-demo.o: sdk/examples/radix-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build ring-buf-demo.efi example
# ===================================================================

ring-buf-demo: $(PREFIX)/ring-buf-demo.efi
	@echo "  Built: $(PREFIX)/ring-buf-demo.efi"

$(PREFIX)/ring-buf-demo.efi: $(BUILDDIR)/ring-buf-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/ring-buf-demo.o,$@)

$(BUILDDIR)/ring-buf-demo.o: sdk/examples/ring-buf-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build event-demo.efi example
# ===================================================================

event-demo: $(PREFIX)/event-demo.efi
	@echo "  Built: $(PREFIX)/event-demo.efi"

$(PREFIX)/event-demo.efi: $(BUILDDIR)/event-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/event-demo.o,$@)

$(BUILDDIR)/event-demo.o: sdk/examples/event-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build cancellable-demo.efi example
# ===================================================================

cancellable-demo: $(PREFIX)/cancellable-demo.efi
	@echo "  Built: $(PREFIX)/cancellable-demo.efi"

$(PREFIX)/cancellable-demo.efi: $(BUILDDIR)/cancellable-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cancellable-demo.o,$@)

$(BUILDDIR)/cancellable-demo.o: sdk/examples/cancellable-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build runtime-demo.efi example (Phase A7 prototype)
# ===================================================================

runtime-demo: $(PREFIX)/runtime-demo.efi
	@echo "  Built: $(PREFIX)/runtime-demo.efi"

$(PREFIX)/runtime-demo.efi: $(BUILDDIR)/runtime-demo.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/runtime-demo.o,$@)

$(BUILDDIR)/runtime-demo.o: sdk/examples/runtime-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-server.efi example (AxlSocket async echo)
# ===================================================================

echo-server: $(PREFIX)/echo-server.efi
	@echo "  Built: $(PREFIX)/echo-server.efi"

$(PREFIX)/echo-server.efi: $(BUILDDIR)/echo-server.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/echo-server.o,$@)

$(BUILDDIR)/echo-server.o: sdk/examples/echo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build tcp-echo-server.efi example (AxlTcp primitives)
# ===================================================================

tcp-echo-server: $(PREFIX)/tcp-echo-server.efi
	@echo "  Built: $(PREFIX)/tcp-echo-server.efi"

$(PREFIX)/tcp-echo-server.efi: $(BUILDDIR)/tcp-echo-server.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/tcp-echo-server.o,$@)

$(BUILDDIR)/tcp-echo-server.o: sdk/examples/tcp-echo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-client.efi example (sync AxlSocket client)
# ===================================================================

echo-client: $(PREFIX)/echo-client.efi
	@echo "  Built: $(PREFIX)/echo-client.efi"

$(PREFIX)/echo-client.efi: $(BUILDDIR)/echo-client.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/echo-client.o,$@)

$(BUILDDIR)/echo-client.o: sdk/examples/echo-client.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-server-sync.efi example (single-client sync AxlSocket)
# ===================================================================

echo-server-sync: $(PREFIX)/echo-server-sync.efi
	@echo "  Built: $(PREFIX)/echo-server-sync.efi"

$(PREFIX)/echo-server-sync.efi: $(BUILDDIR)/echo-server-sync.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/echo-server-sync.o,$@)

$(BUILDDIR)/echo-server-sync.o: sdk/examples/echo-server-sync.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build axl-kernel POC binaries (experimental; x64 + aa64).
# See experiments/axl-kernel/ and docs/AXL-Kernel-Design.md §13.
# ===================================================================

KERNEL_POC_DIR  = experiments/axl-kernel
KERNEL_POC_INC  = -I$(KERNEL_POC_DIR)/include

# Per-arch context-switch source.
ifeq ($(ARCH),aa64)
  KERNEL_CTX_SRC = $(KERNEL_POC_DIR)/src/ctx-switch-aarch64.S
else
  KERNEL_CTX_SRC = $(KERNEL_POC_DIR)/src/ctx-switch-x86_64.S
endif

KERNEL_CORE_OBJS = \
    $(BUILDDIR)/axl-kernel-ctx-switch.o \
    $(BUILDDIR)/axl-kernel.o

KERNEL_POC_OBJS     = $(KERNEL_CORE_OBJS) $(BUILDDIR)/kernel-poc.o
AXLK_ECHO_OBJS      = $(KERNEL_CORE_OBJS) $(BUILDDIR)/axlk-echo-server.o
AXLK_HWINFO_OBJS    = $(KERNEL_CORE_OBJS) $(BUILDDIR)/axlk-hwinfo-server.o
AXLK_BOOTCFG_OBJS   = $(KERNEL_CORE_OBJS) $(BUILDDIR)/axlk-bootconfig-server.o
AXLK_REQLOG_OBJS    = $(KERNEL_CORE_OBJS) $(BUILDDIR)/axlk-reqlog-server.o

kernel-poc: $(PREFIX)/AxlKernelPoc.efi
	@echo "  Built: $(PREFIX)/AxlKernelPoc.efi"

axlk-echo-server: $(PREFIX)/axlk-echo-server.efi
	@echo "  Built: $(PREFIX)/axlk-echo-server.efi"

axlk-hwinfo-server: $(PREFIX)/axlk-hwinfo-server.efi
	@echo "  Built: $(PREFIX)/axlk-hwinfo-server.efi"

axlk-bootconfig-server: $(PREFIX)/axlk-bootconfig-server.efi
	@echo "  Built: $(PREFIX)/axlk-bootconfig-server.efi"

axlk-reqlog-server: $(PREFIX)/axlk-reqlog-server.efi
	@echo "  Built: $(PREFIX)/axlk-reqlog-server.efi"

$(PREFIX)/AxlKernelPoc.efi: $(KERNEL_POC_OBJS) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(KERNEL_POC_OBJS),$@)

$(PREFIX)/axlk-echo-server.efi: $(AXLK_ECHO_OBJS) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_ECHO_OBJS),$@)

$(PREFIX)/axlk-hwinfo-server.efi: $(AXLK_HWINFO_OBJS) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_HWINFO_OBJS),$@)

$(PREFIX)/axlk-bootconfig-server.efi: $(AXLK_BOOTCFG_OBJS) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_BOOTCFG_OBJS),$@)

$(PREFIX)/axlk-reqlog-server.efi: $(AXLK_REQLOG_OBJS) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_REQLOG_OBJS),$@)

$(BUILDDIR)/axl-kernel-ctx-switch.o: $(KERNEL_CTX_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

$(BUILDDIR)/axl-kernel.o: $(KERNEL_POC_DIR)/src/kernel.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@

$(BUILDDIR)/kernel-poc.o: $(KERNEL_POC_DIR)/test/kernel-poc.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@

$(BUILDDIR)/axlk-echo-server.o: $(KERNEL_POC_DIR)/test/axlk-echo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@

$(BUILDDIR)/axlk-hwinfo-server.o: $(KERNEL_POC_DIR)/test/axlk-hwinfo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@

$(BUILDDIR)/axlk-bootconfig-server.o: $(KERNEL_POC_DIR)/test/axlk-bootconfig-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@

$(BUILDDIR)/axlk-reqlog-server.o: $(KERNEL_POC_DIR)/test/axlk-reqlog-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(KERNEL_POC_INC) -c $< -o $@


# ===================================================================
# Build test applications (all modules)
# ===================================================================

TEST_CFLAGS = $(CFLAGS) $(INCLUDES) -Itest/unit -Itest/data -Isrc/ata -Isrc/hii -Isrc/net

TESTS = AxlTestMem AxlTestString AxlTestIO AxlTestLog \
        AxlTestData AxlTestUtil AxlTestLoop AxlTestTask AxlTestNet \
        AxlTestSmbus AxlTestIpmi AxlTestPlatform AxlTestEvent \
        AxlTestCpuIdle AxlTestRuntime AxlTestXml AxlTestFsProvider \
        AxlTestGfx AxlTestTruetype AxlTestPixmap AxlTestMath \
        AxlTestInput AxlTestFileView AxlTestPieceTree AxlTestFind \
        AxlTestDriver AxlTestCursor AxlTestCompositor AxlTestGfxRegion \
        AxlTestCrypto AxlTestJose AxlTestNvme AxlTestAta AxlTestScsi AxlTestSmart \
        AxlTestHii AxlTestAuth AxlTestFw AxlTestVterm AxlTest9p \
        AxlTestJsonConformance AxlTestJsonCorpus

TEST_EFIS = $(patsubst %,$(PREFIX)/%.efi,$(TESTS))

tests: all $(TEST_EFIS) $(PREFIX)/axl-shell-launcher.efi
	@echo "  Built $(words $(TESTS)) test EFIs"

# Helper: compile test source, link with libaxl, output .efi
define BUILD_TEST
$(PREFIX)/$(1).efi: $(BUILDDIR)/$(2).o $(PREFIX)/lib/libaxl.a
	$$(call LINK_EFI_TEST,$(BUILDDIR)/$(2).o,$$@)

$(BUILDDIR)/$(2).o: test/unit/$(2).c | $(BUILDDIR)
	$(CC) $(TEST_CFLAGS) -c $$< -o $$@
endef

$(eval $(call BUILD_TEST,AxlTestMem,axl-test-mem))
$(eval $(call BUILD_TEST,AxlTestString,axl-test-string))
$(eval $(call BUILD_TEST,AxlTestIO,axl-test-io))
$(eval $(call BUILD_TEST,AxlTestLog,axl-test-log))
$(eval $(call BUILD_TEST,AxlTestData,axl-test-data))
$(eval $(call BUILD_TEST,AxlTestUtil,axl-test-util))
$(eval $(call BUILD_TEST,AxlTestLoop,axl-test-loop))
$(eval $(call BUILD_TEST,AxlTestTask,axl-test-task))
$(eval $(call BUILD_TEST,AxlTestNet,axl-test-net))
$(eval $(call BUILD_TEST,AxlTestSmbus,axl-test-smbus))
$(eval $(call BUILD_TEST,AxlTestIpmi,axl-test-ipmi))
$(eval $(call BUILD_TEST,AxlTestPlatform,axl-test-platform))
$(eval $(call BUILD_TEST,AxlTestEvent,axl-test-event))
$(eval $(call BUILD_TEST,AxlTestCpuIdle,axl-test-cpu-idle))
$(eval $(call BUILD_TEST,AxlTestRuntime,axl-test-runtime))
$(eval $(call BUILD_TEST,AxlTestXml,axl-test-xml))
$(eval $(call BUILD_TEST,AxlTestFsProvider,axl-test-fs-provider))
$(eval $(call BUILD_TEST,AxlTestGfx,axl-test-gfx))
$(eval $(call BUILD_TEST,AxlTestTruetype,axl-test-truetype))
$(eval $(call BUILD_TEST,AxlTestPixmap,axl-test-pixmap))
$(eval $(call BUILD_TEST,AxlTestMath,axl-test-math))
$(eval $(call BUILD_TEST,AxlTestInput,axl-test-input))
$(eval $(call BUILD_TEST,AxlTestFileView,axl-test-file-view))
$(eval $(call BUILD_TEST,AxlTestPieceTree,axl-test-piece-tree))
$(eval $(call BUILD_TEST,AxlTestFind,axl-test-find))
$(eval $(call BUILD_TEST,AxlTestVterm,axl-test-vterm))
$(eval $(call BUILD_TEST,AxlTestDriver,axl-test-driver))
$(eval $(call BUILD_TEST,AxlTestCursor,axl-test-cursor))
$(eval $(call BUILD_TEST,AxlTestCompositor,axl-test-compositor))
$(eval $(call BUILD_TEST,AxlTestGfxRegion,axl-test-gfx-region))
$(eval $(call BUILD_TEST,AxlTestCrypto,axl-test-crypto))
$(eval $(call BUILD_TEST,AxlTestJose,axl-test-jose))
$(eval $(call BUILD_TEST,AxlTestNvme,axl-test-nvme))
$(eval $(call BUILD_TEST,AxlTestAta,axl-test-ata))
$(eval $(call BUILD_TEST,AxlTestScsi,axl-test-scsi))
$(eval $(call BUILD_TEST,AxlTestSmart,axl-test-smart))
$(eval $(call BUILD_TEST,AxlTestHii,axl-test-hii))
$(eval $(call BUILD_TEST,AxlTestAuth,axl-test-auth))
$(eval $(call BUILD_TEST,AxlTestFw,axl-test-fw))
$(eval $(call BUILD_TEST,AxlTest9p,axl-test-9p))
$(eval $(call BUILD_TEST,AxlTestJsonConformance,axl-test-json-conformance))
$(eval $(call BUILD_TEST,AxlTestJsonCorpus,axl-test-json-corpus))

# ===================================================================
# Tools (standalone UEFI utilities)
# ===================================================================

TOOL_NAMES = hexdump fetch find grep sed cat sysinfo netinfo mkrd rfbrowse ipmi dmidecode memspd lspci lsusb mkfixture rndisfix timetest i2c clip paste tar nvme ata scsi smart fwtool axbench kbtune netload lsproto cut tr
TOOL_EFIS  = $(patsubst %,$(PREFIX)/tools/%.efi,$(TOOL_NAMES))

tools: all $(TOOL_EFIS) $(PREFIX)/tools/kbtune-drv.efi $(PREFIX)/tools/fbcon.efi $(PREFIX)/tools/crashtest.efi $(PREFIX)/drivers/crashhandler.efi $(PREFIX)/tools/9p.efi
	@echo "  Built $(words $(TOOL_NAMES)) tools + kbtune-drv + fbcon + crashtest + crashhandler driver + 9p"

# tool-sizes — print per-tool .efi size, sorted ascending.
# Surfaces the selective-linking benefit: a tool that uses 5% of
# libaxl.a carries only that 5% (per-symbol via --gc-sections plus
# the static-archive's per-.o member resolution). Run after `make
# tools` to see what each tool weighs in at.
.PHONY: tool-sizes
tool-sizes: tools
	@echo ""
	@echo "Tool .efi sizes ($(ARCH)):"
	@for t in $(TOOL_NAMES); do \
	    f=$(PREFIX)/tools/$$t.efi; \
	    if [ -f $$f ]; then \
	        sz=$$(stat -c %s $$f); \
	        printf "  %7d B   %s\n" "$$sz" "$$t"; \
	    fi; \
	done | sort -n
	@total=$$(stat -c %s $(TOOL_EFIS) | awk '{s+=$$1} END {print s}'); \
	    echo "  -------------------------"; \
	    printf "  %7d B   total\n" "$$total"; \
	    libsz=$$(stat -c %s $(PREFIX)/lib/libaxl.a); \
	    printf "  (libaxl.a archive: %d B — selective linking pulls only \
referenced symbols)\n" "$$libsz"

# Embedded driver blob for mkrd. Vendored EDK2 RamDiskDxe.efi (one per
# arch) is embedded into mkrd.efi via the GNU assembler's `.incbin`
# directive — see the EMBED_BLOB macro near the top. mkrd LoadImages
# it from memory when the host firmware doesn't ship
# EFI_RAM_DISK_PROTOCOL. See third_party/edk2/README.md for provenance
# and license.
EMBEDDED_RAMDISK_SRC = third_party/edk2/RamDiskDxe-$(ARCH).efi
$(eval $(call EMBED_BLOB,ramdiskdxe,$(EMBEDDED_RAMDISK_SRC)))
EMBEDDED_RAMDISK_OBJ = $(BLOB_OBJ_ramdiskdxe)

# ===================================================================
# Build volume-map-test.efi — regression test that axl_volume_enumerate
# names volumes from the UEFI Shell fsN map, not the LocateHandle index.
# Placed after EMBEDDED_RAMDISK_OBJ is defined (it links the vendored
# RamDiskDxe blob, like mkrd, for the mkrd phase).
# ===================================================================

volume-map-test: $(PREFIX)/volume-map-test.efi
	@echo "  Built: $(PREFIX)/volume-map-test.efi"

$(PREFIX)/volume-map-test.efi: $(BUILDDIR)/volume-map-test.o $(EMBEDDED_RAMDISK_OBJ) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/volume-map-test.o $(EMBEDDED_RAMDISK_OBJ),$@)

$(BUILDDIR)/volume-map-test.o: test/integration/volume-map-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

define BUILD_TOOL
$(PREFIX)/tools/$(1).efi: $(BUILDDIR)/$(1).o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$$(call LINK_EFI_APP,$(BUILDDIR)/$(1).o,$$@)

$(BUILDDIR)/$(1).o: tools/$(1).c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $$< -o $$@
endef

# mkrd is linked alongside the .incbin'd blob object — filter it out
# of the generic foreach so BUILD_TOOL doesn't generate a recipe that
# the special rule below would have to override. Without this filter
# `make` warns "overriding recipe for target mkrd.efi" on every build.
$(foreach t,$(filter-out mkrd,$(TOOL_NAMES)),$(eval $(call BUILD_TOOL,$(t))))

$(PREFIX)/tools/mkrd.efi: $(BUILDDIR)/mkrd.o $(EMBEDDED_RAMDISK_OBJ) \
                         $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(BUILDDIR)/mkrd.o $(EMBEDDED_RAMDISK_OBJ),$@)

$(BUILDDIR)/mkrd.o: tools/mkrd.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# 9p -- 9P2000.L client/server launcher. A first-class tool with its
# own recipe (not in TOOL_NAMES) because it links embedded DXE driver
# blobs, which the busybox multiplexer's one-.o-per-tool rule cannot
# express.
#
# 9p-serve-svc.c and 9p-mount-svc.c are each compiled TWICE, the
# service-demo pattern: with -DAXL_SERVICE_BUILD_DRIVER into a driver
# image (subsystem 11), and without it into the launcher. The driver
# .efi files are BUILDDIR intermediates -- they ship only inside
# 9p.efi, so nothing stages them separately.
# ===================================================================
9p: $(PREFIX)/tools/9p.efi
	@echo "  Built: $(PREFIX)/tools/9p.efi (launcher + embedded serve/mount drivers)"

NINEP_HDRS = tools/9p-common.h tools/9p-serve-svc.h tools/9p-mount-svc.h

# Launcher objects.
NINEP_APP_OBJS = $(BUILDDIR)/9p.o $(BUILDDIR)/9p-common.o \
                 $(BUILDDIR)/9p-cmd-file.o $(BUILDDIR)/9p-cmd-serve.o \
                 $(BUILDDIR)/9p-serve-app.o $(BUILDDIR)/9p-cmd-mount.o \
                 $(BUILDDIR)/9p-mount-app.o

# Intermediates that clean-tools must reap alongside NINEP_APP_OBJS.
NINEP_DRV_OBJS = $(BUILDDIR)/9p-serve-dxe.o $(BUILDDIR)/9p-serve-dxe.efi \
                 $(BUILDDIR)/9p-serve-dxe.so \
                 $(BUILDDIR)/9p-mount-dxe.o $(BUILDDIR)/9p-mount-dxe.efi \
                 $(BUILDDIR)/9p-mount-dxe.so

$(BUILDDIR)/9p-serve-dxe.efi: $(BUILDDIR)/9p-serve-dxe.o $(PREFIX)/lib/libaxl.a | $(BUILDDIR)
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/9p-serve-dxe.o,$@)

$(BUILDDIR)/9p-serve-dxe.o: tools/9p-serve-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(BUILDDIR)/9p-serve-app.o: tools/9p-serve-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-serve.o: tools/9p-cmd-serve.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-mount-dxe.efi: $(BUILDDIR)/9p-mount-dxe.o $(PREFIX)/lib/libaxl.a | $(BUILDDIR)
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/9p-mount-dxe.o,$@)

$(BUILDDIR)/9p-mount-dxe.o: tools/9p-mount-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DAXL_SERVICE_BUILD_DRIVER -c $< -o $@

$(BUILDDIR)/9p-mount-app.o: tools/9p-mount-svc.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-mount.o: tools/9p-cmd-mount.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-common.o: tools/9p-common.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Embed symbols axl_embedded_{serve,mount}9p_dxe match the matching
# AXL_EMBED_DECLARE in 9p-cmd-serve.c / 9p-cmd-mount.c. The blob NAMEs
# deliberately do not lead with a digit: AXL_EMBED_DECLARE token-pastes them
# onto axl_embedded_, and a leading digit makes that paste a
# preprocessing-number rather than plainly an identifier.
$(eval $(call EMBED_BLOB,serve9p_dxe,$(BUILDDIR)/9p-serve-dxe.efi))
$(eval $(call EMBED_BLOB,mount9p_dxe,$(BUILDDIR)/9p-mount-dxe.efi))

$(PREFIX)/tools/9p.efi: $(NINEP_APP_OBJS) \
                        $(BLOB_OBJ_serve9p_dxe) $(BLOB_OBJ_mount9p_dxe) \
                        $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(NINEP_APP_OBJS) \
	                    $(BLOB_OBJ_serve9p_dxe) $(BLOB_OBJ_mount9p_dxe),$@)

$(BUILDDIR)/9p.o: tools/9p.c $(NINEP_HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/9p-cmd-file.o: tools/9p-cmd-file.c tools/9p-common.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# kbtune-drv — resident ConIn conditioning shim, paired with the kbtune tool.
# Built as an EFI DRIVER (DriverEntry, subsystem 11) and staged in tools/ so the
# kbtune launcher finds it as a SIBLING via axl_shared_driver_locate_sibling.
# Not in TOOL_NAMES (it is a driver, not a busybox subcommand).
kbtune-drv: $(PREFIX)/tools/kbtune-drv.efi
	@echo "  Built: $(PREFIX)/tools/kbtune-drv.efi"

$(PREFIX)/tools/kbtune-drv.efi: $(BUILDDIR)/kbtune-drv.o $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/kbtune-drv.o,$@)

$(BUILDDIR)/kbtune-drv.o: tools/kbtune-drv.c tools/kbtune-shared.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# fbcon — graphical terminal take-over of the running Shell. Ships as ONE runnable
# `fbcon.efi` APP (subsystem 10) that EMBEDS the resident take-over driver
# (fbcon-drv.efi, subsystem 11) and loads it from memory -- so the user runs
# `fbcon.efi` as a command instead of `load`-ing a driver. The launcher reaps any
# prior resident instance (a driver can't self-unload; the separate launcher can),
# then starts a fresh one. Same embed pattern as do.efi / mkrd. fbcon-drv is not
# staged separately (it lives inside the app). Not in TOOL_NAMES (not a busybox verb).
fbcon: $(PREFIX)/tools/fbcon.efi
	@echo "  Built: $(PREFIX)/tools/fbcon.efi (launcher + embedded fbcon-drv)"

# The resident take-over driver, embedded into the launcher below. Built as a build
# intermediate (not staged in tools/) -- it ships only inside fbcon.efi.
$(BUILDDIR)/fbcon-drv.efi: $(BUILDDIR)/fbcon-drv.o $(PREFIX)/lib/libaxl.a | $(BUILDDIR)
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/fbcon-drv.o,$@)

$(BUILDDIR)/fbcon-drv.o: tools/fbcon-drv.c tools/fbcon-marker.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Embed the driver blob (emits axl_embedded_fbcon_drv{,_end} for AXL_EMBED_DECLARE).
$(eval $(call EMBED_BLOB,fbcon_drv,$(BUILDDIR)/fbcon-drv.efi))

$(PREFIX)/tools/fbcon.efi: $(BUILDDIR)/fbcon.o $(BLOB_OBJ_fbcon_drv) $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$(call LINK_EFI_APP,$(BUILDDIR)/fbcon.o $(BLOB_OBJ_fbcon_drv),$@)

$(BUILDDIR)/fbcon.o: tools/fbcon.c tools/fbcon-marker.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# kbtune-drv-test — hazard-safe lifecycle test for kbtune-drv (load / get-set /
# unload-restore). Public headers only + tools/kbtune-shared.h for the config
# contract (-Itools). Run in isolation by test-kbtune-driver-qemu.sh.
kbtune-drv-test: $(PREFIX)/kbtune-drv-test.efi
	@echo "  Built: $(PREFIX)/kbtune-drv-test.efi"

$(PREFIX)/kbtune-drv-test.efi: $(BUILDDIR)/kbtune-drv-test.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/kbtune-drv-test.o,$@)

$(BUILDDIR)/kbtune-drv-test.o: test/integration/kbtune-drv-test.c tools/kbtune-shared.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Itools -c $< -o $@

$(PREFIX)/tools:
	@mkdir -p $@

# ===================================================================
# axl-busybox — single-binary build hosting all tools as subcommands
# ===================================================================
#
# Opt-in alternative deployment shape: one axl.efi instead of 18
# per-tool .efi files. Same source files; each tool .c is recompiled
# with -DAXL_BUSYBOX so its AXL_TOOL_MAIN(name) macro emits
# axl_tool_<name>_main() instead of main(). tools/axl.c is the
# dispatcher that argv[1]-routes to the matching tool.
#
# Default `make tools` is unaffected — it still produces individual
# .efi binaries from the same sources without -DAXL_BUSYBOX.

BUSYBOX_DIR  = $(BUILDDIR)/busybox
BUSYBOX_OBJS = $(patsubst %,$(BUSYBOX_DIR)/%.o,$(TOOL_NAMES) axl)
BUSYBOX_EFI  = $(PREFIX)/axl.efi

# Same CFLAGS as standalone tools, with AXL_BUSYBOX defined so
# AXL_TOOL_MAIN expands to a uniquely-named function per tool.
$(BUSYBOX_DIR)/%.o: tools/%.c | $(BUSYBOX_DIR)
	$(CC) $(CFLAGS) -DAXL_BUSYBOX $(INCLUDES) -c $< -o $@

$(BUSYBOX_DIR):
	@mkdir -p $@

# mkrd's embedded RamDiskDxe blob comes along even in the busybox
# build — it's small (KB) and lets `axl mkrd` work the same as
# standalone mkrd.efi.
$(BUSYBOX_EFI): $(BUSYBOX_OBJS) $(EMBEDDED_RAMDISK_OBJ) \
                $(LINK_CRT0) $(PREFIX)/lib/libaxl.a | $(PREFIX)
	$(call LINK_EFI_APP,$(BUSYBOX_OBJS) $(EMBEDDED_RAMDISK_OBJ),$@)

axl-busybox: all $(BUSYBOX_EFI)
	@echo ""
	@echo "  axl busybox built ($(ARCH))"
	@echo "  Binary:    $(BUSYBOX_EFI)"
	@printf  "  Size:      %d B\n" "$$(stat -c %s $(BUSYBOX_EFI))"
	@echo "  Try:       axl --help"
	@echo "             axl <tool> --help"
	@echo ""

.PHONY: axl-busybox

# ===================================================================
# Clean
# ===================================================================

# ===================================================================
# Automatic dependency tracking (per-object .d generated by -MD -MP; -MP adds
# phony header rules so a deleted/renamed header doesn't break the build)
# ===================================================================

-include $(wildcard $(BUILDDIR)/*.d)

# Report this configuration's output directory, so a caller that builds with
# a non-default BUILD/ARCH can find the artefacts without duplicating the
# naming rule (see the PREFIX block at the top).
#   cp "$(make -s ARCH=x64 BUILD=RELEASE print-prefix)/tools/"*.efi ...
print-prefix:
	@echo $(PREFIX)

# Removes THIS configuration's tree only ($(PREFIX)); a different BUILD or
# ARCH keeps its own. `clean-all` wipes every tree.
clean:
	rm -rf $(PREFIX)

clean-all:
	rm -rf out

# Targeted clean for tool binaries (uefi-devkit references this from
# its `tools-clean` recipe — it doesn't want to wipe libaxl.a). Also
# drops the per-tool .o files since BUILD_TOOL puts them in $(BUILDDIR).
clean-tools:
	rm -f $(PREFIX)/tools/*.efi $(PREFIX)/tools/*.so
	@for t in $(TOOL_NAMES); do rm -f $(BUILDDIR)/$$t.o; done
	rm -f $(NINEP_APP_OBJS) $(NINEP_DRV_OBJS) \
	      $(BLOB_OBJ_serve9p_dxe) $(BLOB_SRC_serve9p_dxe) \
	      $(BLOB_OBJ_mount9p_dxe) $(BLOB_SRC_mount9p_dxe)
	rm -f $(BUSYBOX_EFI) $(PREFIX)/axl.so
	rm -rf $(BUSYBOX_DIR)

# Verification driver for axl_service_reload (self-reload via the SDK built-in).
reload-svc-dxe: $(PREFIX)/reload-svc-dxe.efi
	@echo "  Built: $(PREFIX)/reload-svc-dxe.efi"

$(PREFIX)/reload-svc-dxe.efi: $(BUILDDIR)/reload-svc-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/reload-svc-dxe.o,$@)

$(BUILDDIR)/reload-svc-dxe.o: sdk/examples/reload-svc-dxe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Poisoned replacement (same service name, setup fails on purpose) — proves
# axl_service_reload reports a start failure instead of a healthy hot-swap.
reload-svc-fail-dxe: $(PREFIX)/reload-svc-fail-dxe.efi
	@echo "  Built: $(PREFIX)/reload-svc-fail-dxe.efi"

$(PREFIX)/reload-svc-fail-dxe.efi: $(BUILDDIR)/reload-svc-fail-dxe.o $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_DRIVER,$(BUILDDIR)/reload-svc-fail-dxe.o,$@)

$(BUILDDIR)/reload-svc-fail-dxe.o: sdk/examples/reload-svc-fail-dxe.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
