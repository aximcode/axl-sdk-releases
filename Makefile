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
ifeq ($(BUILD),DEBUG)
  PREFIX   ?= out/native-$(ARCH)
else
  PREFIX   ?= out/native-$(ARCH)-$(shell echo $(BUILD) | tr '[:upper:]' '[:lower:]')
endif
HOSTCC     ?= gcc
AXL_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)

# ===================================================================
# Toolchain: gcc
# ===================================================================

ifeq ($(ARCH),aa64)
  CROSS      = aarch64-linux-gnu-
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
  CROSS      =
  GCC_ARCH   = -mno-red-zone -march=x86-64
  EFI_LDS    = scripts/elf_x86_64_efi.lds
  PE_TARGET  = pei-x86-64
endif

CC         = $(CROSS)gcc
LD_ELF     = $(CROSS)ld
AR         = $(CROSS)ar
OBJCOPY    = $(CROSS)objcopy

# C++ toolchain — only consulted when AXL_CPP=1.  AArch64 uses ARM's
# bare-metal "none-elf" cross because the Linux-ABI cross's libstdc++
# headers pull hosted typedefs from <bits/c++config.h> (see
# docs/ROADMAP.md axlmm spec).  X64 uses host g++ (same convention as
# axl-cc's host gcc).  Pinned version matches Dell ePSA per CPP1
# validation findings.
ifdef AXL_CPP
  ifeq ($(ARCH),aa64)
    ARM_TOOLCHAIN ?= /opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
    CXX = $(ARM_TOOLCHAIN)/bin/aarch64-none-elf-g++
    ifeq ($(wildcard $(CXX)),)
      $(error AXL_CPP=1 needs $(CXX) — run ./scripts/install-arm-toolchain.sh)
    endif
  else
    CXX = g++
    ifeq ($(shell command -v g++ 2>/dev/null),)
      $(error AXL_CPP=1 needs g++ in PATH)
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

CFLAGS_BASE = -std=gnu2x \
              -ffreestanding -fshort-wchar \
              -fno-stack-protector -fno-builtin \
              -fno-math-errno -fno-trapping-math \
              -fno-omit-frame-pointer \
              -fpic $(GCC_ARCH) \
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

# C++ flag set for libaxl-cxx.a (and any future C++ source under src/).
# Matches the hard defaults baked into axl-cc's C++ path — no
# exceptions, no RTTI, no thread-safe statics, C++20 minimum.  See
# docs/ROADMAP.md "axlmm Toolchain" section.
ifdef AXL_CPP
CXXFLAGS_BASE = -std=c++20 \
                -ffreestanding -fshort-wchar \
                -fno-stack-protector -fno-builtin \
                -fno-omit-frame-pointer \
                -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                -fpic $(GCC_ARCH) \
                -Wall \
                -DAXL_BACKEND_NATIVE
CXXFLAGS      = $(CXXFLAGS_BASE) $(CFLAGS_BUILD)
endif

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

OBJCOPY_SECTIONS = -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym \
                   -j .rel -j .rela -j .reloc -j .rodata -j .dbgdir

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
INCLUDES   = -Iinclude -Iinclude/compat -Isrc/backend -Ideps/lzma \
             -Ideps/libvterm/include

# ===================================================================
# Optional TLS support (AXL_TLS=1)
# ===================================================================

ifdef AXL_TLS
CFLAGS += -DAXL_HAVE_TLS -Ideps/mbedtls/include \
          -DMBEDTLS_CONFIG_FILE='<axl-mbedtls-config.h>' \
          -Isrc/net -Iinclude/compat -Wno-redundant-decls \
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
    src/log/axl-log.c \
    src/log/axl-log-ring.c \
    src/log/axl-log-file.c \
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
    src/data/axl-json5-parse.c \
    src/data/axl-json-build.c \
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
LIB_CXX_SOURCES = src/runtime/axl-cxxabi-ops.cpp
LIB_CXX_OBJS    = $(patsubst %.cpp,$(BUILDDIR)/%.o,$(notdir $(LIB_CXX_SOURCES)))

ifdef AXL_CPP
LIBAXL_CXX_TARGET = $(PREFIX)/lib/libaxl-cxx.a
else
LIBAXL_CXX_TARGET =
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
# The pure-lint gates (check-ascii/-docs/-test-meta/-dogfood/-cxx-entry) build no
# libaxl.a and leave no binary that could go stale against its ABI, but they run
# WITHOUT AXL_TLS — so a bare `make check-ascii` after an AXL_TLS=1 build used to
# read TLS_STATE=off, see the toggle, and WIPE the TLS tree (out/native-<arch>),
# forcing a full rebuild. Exclude them (like clean/help) so a lint neither wipes
# the tree nor rewrites the recorded state to confuse the next real build.
NONCLEAN_GOALS := $(filter-out clean clean-all clean-tools help check-version \
    print-prefix \
    check-ascii check-docs check-test-meta check-dogfood check-cxx-entry,\
    $(or $(MAKECMDGOALS),all))

ifneq ($(NONCLEAN_GOALS),)
TLS_STATE := $(if $(AXL_TLS),on,off)
TLS_STATE_FILE := $(BUILDDIR)/.axl-tls-state
PREV_TLS_STATE := $(shell cat $(TLS_STATE_FILE) 2>/dev/null)

ifneq ($(TLS_STATE),$(PREV_TLS_STATE))
ifneq ($(PREV_TLS_STATE),)
$(info AXL_TLS state changed: $(PREV_TLS_STATE) -> $(TLS_STATE); wiping .o, libaxl.a, all .efi/.so under $(PREFIX) to avoid stale-archive linkage)
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
$(shell rm -f $(BUILDDIR)/*.o $(PREFIX)/lib/libaxl.a $(PREFIX)/*.efi $(PREFIX)/*.so $(PREFIX)/tools/*.efi $(PREFIX)/tools/*.so $(PREFIX)/drivers/*.efi $(PREFIX)/drivers/*.so)
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

.PHONY: all clean clean-all clean-tools print-prefix hello gfx-demo gfx-window pointer-demo pointer-tune-demo cursor-demo frame-anim-demo keytrace input-demo driver smbus-hc-shim binding-driver crashhandler crashtest radix-demo ring-buf-demo event-demo cancellable-demo runtime-demo echo-server tcp-echo-server echo-client echo-server-sync kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server tests tools check-version check-ascii check-cxx-entry check-test-meta check-docs check-dogfood check-nx-compat check-bss-clear driver-leak-test driver-identity-test driver-parent-leak-test volume-map-test stdio-bridge-reap-test stdio-bridge-liveness-test stdio-bridge-fix stdio-bridge-self stdio-bridge-leak sd-ergo sd-sibling sd-sibling-probe sd-sibling-driver-a sd-sibling-driver-b io-streams cpu-spin-fixture service-demo service-demo-custom svc-startfail svc-embonly embed-asset gfx-present-selftest gfx-avail-probe cursor-selftest exit-status-selftest exit-status-selftest-minimal compositor-selftest compositor-bench cpu-simd-selftest cpu-topology-selftest task-pool-mp-selftest time-settime-selftest http-plain-selftest gfx-simd-selftest console-text-mode-selftest console-reshape-selftest console-device-smoke console-device-restore-smoke console-device-wide-smoke console-device-input-smoke console-device-input-restore-smoke console-device-wide-restore-smoke console-device-cycle-smoke fs-path-selftest fs-read kbprobe axbench kbtune-drv kbtune-drv-test fbcon pin-svc image-path-test shell-launcher 9p 9p-mount-selftest 9p-server-selftest flushfail-fs-driver console-device-passthrough-smoke

# Pin the default goal so rule order can't turn check-version (or
# any future helper target) into the default by accident.
.DEFAULT_GOAL := all

all: check-version $(PREFIX)/lib/libaxl.a $(LIBAXL_CXX_TARGET) $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(CRT0_OBJ) $(CRT0_MINIMAL_OBJ) $(PE_SET_DEBUG)
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
	$(CXX) $(CXXFLAGS_BASE) -Iinclude -Iinclude/compat -c test/cxx-entry-linkage.cpp -o $$obj || \
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
# (the only .cpp source in libaxl-cxx.a today lives here).
ifdef AXL_CPP
$(BUILDDIR)/%.o: src/runtime/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
endif

$(BUILDDIR)/%.o: src/crt0/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/crt0/%.S | $(BUILDDIR)
	$(CC) $(CFLAGS_BASE) -c $< -o $@

$(BUILDDIR)/%.o: deps/lzma/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DZ7_ST -c $< -o $@

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
        AxlTestHii AxlTestAuth AxlTestFw AxlTestVterm AxlTest9p

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
