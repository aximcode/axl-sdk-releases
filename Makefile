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
PREFIX     ?= out/native-$(ARCH)
TYPE       ?= app
BUILD      ?= DEBUG
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
LDFLAGS_EFI = -nostdlib -shared -Bsymbolic --no-warn-rwx-segments --no-undefined \
              --gc-sections

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

OBJCOPY_SECTIONS = -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
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
define EMBED_BLOB
BLOB_OBJ_$(1) := $$(BUILDDIR)/embed-blob-$(1).o

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

CFLAGS     = $(CFLAGS_BASE) $(CFLAGS_BUILD) -MD
INCLUDES   = -Iinclude -Iinclude/compat -Isrc/backend

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
    src/data/axl-hmac.c \
    src/data/axl-bytes.c \
    src/stream/axl-stream.c \
    src/stream/axl-stream-buf.c \
    src/stream/axl-stream-file.c \
    src/stream/axl-stream-text.c \
    src/stream/axl-compress-stream.c \
    src/fs/axl-fs.c \
    src/fs/axl-file-writer.c \
    src/fs/axl-file-view.c \
    src/fs/axl-fs-provider.c \
    src/fs/axl-device-path.c \
    src/util/axl-debug.c \
    src/util/axl-path.c \
    src/util/axl-hexdump.c \
    src/util/axl-time.c \
    src/util/axl-env.c \
    src/util/axl-sys.c \
    src/util/axl-nvstore.c \
    src/util/axl-port.c \
    src/util/axl-boot.c \
    src/util/axl-image.c \
    src/util/axl-shell.c \
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
    src/tpm/axl-tpm.c \
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
    src/runtime/axl-signal.c

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
NONCLEAN_GOALS := $(filter-out clean clean-tools help check-version,$(or $(MAKECMDGOALS),all))

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

.PHONY: all clean clean-tools hello gfx-demo gfx-window pointer-demo pointer-tune-demo cursor-demo frame-anim-demo keytrace input-demo driver smbus-hc-shim binding-driver crashhandler crashtest radix-demo ring-buf-demo event-demo cancellable-demo runtime-demo echo-server tcp-echo-server echo-client echo-server-sync kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server tests tools check-version check-ascii check-test-meta check-docs check-nx-compat driver-leak-test service-demo service-demo-custom embed-asset gfx-present-selftest cursor-selftest exit-status-selftest exit-status-selftest-minimal compositor-selftest compositor-bench cpu-simd-selftest cpu-topology-selftest task-pool-mp-selftest time-settime-selftest http-plain-selftest gfx-simd-selftest console-text-mode-selftest axbench

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

# Every QEMU integration test must carry a `# test-meta:` header so it is
# discovered + sharded by run-integration.sh (a new test can't silently escape
# the suite). Run in CI's lint job alongside check-ascii.
check-test-meta:
	@bash test/integration/lib/discover.sh --lint

# Verify every public header is wired into the Sphinx reference (catches a
# new header landing without docs). Prose staleness is a workflow concern.
check-docs:
	@python3 scripts/check-doc-coverage.py

# Verify the PE post-processor stamped NX_COMPAT (the produced images are
# W^X-clean, so they must advertise NX compatibility for Secure-Boot /
# memory-protected firmware). Builds one representative app + driver and checks
# both — the bit is set uniformly by pe-set-debug, so a regression fails here.
check-nx-compat: $(PREFIX)/cpu-topology-selftest.efi $(PREFIX)/SmbusHcShim.efi
	@python3 scripts/check-pe-nx.py \
	    $(PREFIX)/cpu-topology-selftest.efi $(PREFIX)/SmbusHcShim.efi

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

$(BUILDDIR)/%.o: src/tpm/%.c | $(BUILDDIR)
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

ifdef AXL_TLS
$(BUILDDIR)/%.o: deps/mbedtls/library/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
endif

# Host tool: pe-set-debug (built with host compiler, not cross-compiler)
$(PE_SET_DEBUG): scripts/pe-set-debug.c | $(BUILDDIR)
	$(HOSTCC) -Wall -O2 -o $@ $<

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
# Build axbench.efi — real-hardware AP-pool benchmark tool. Measures
# AxlTaskPool (AP offload) vs the BSP across 8 scenarios; writes a report
# to stdout or a file. Ctrl-C aborts cleanly (test-axbench-ctrlc-qemu.sh).
# Run: scripts/run-qemu.sh --qemu-arg -smp --qemu-arg N axbench.efi
# ===================================================================

axbench: $(PREFIX)/axbench.efi
	@echo "  Built: $(PREFIX)/axbench.efi"

$(PREFIX)/axbench.efi: $(BUILDDIR)/axbench.o $(LINK_CRT0) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/axbench.o,$@)

$(BUILDDIR)/axbench.o: tools/axbench.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

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

TEST_CFLAGS = $(CFLAGS) $(INCLUDES) -Itest/unit -Itest/data -Isrc/ata

TESTS = AxlTestMem AxlTestString AxlTestIO AxlTestLog \
        AxlTestData AxlTestUtil AxlTestLoop AxlTestTask AxlTestNet \
        AxlTestSmbus AxlTestIpmi AxlTestPlatform AxlTestEvent \
        AxlTestCpuIdle AxlTestRuntime AxlTestXml AxlTestFsProvider \
        AxlTestGfx AxlTestTruetype AxlTestPixmap AxlTestMath \
        AxlTestInput AxlTestFileView AxlTestPieceTree AxlTestFind \
        AxlTestDriver AxlTestCursor AxlTestCompositor AxlTestGfxRegion \
        AxlTestCrypto AxlTestJose AxlTestNvme AxlTestAta AxlTestScsi AxlTestSmart

TEST_EFIS = $(patsubst %,$(PREFIX)/%.efi,$(TESTS))

tests: all $(TEST_EFIS)
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

# ===================================================================
# Tools (standalone UEFI utilities)
# ===================================================================

TOOL_NAMES = hexdump fetch find grep cat sysinfo netinfo mkrd rfbrowse ipmi dmidecode memspd lspci lsusb mkfixture rndisfix timetest i2c clip paste tar nvme ata scsi smart
TOOL_EFIS  = $(patsubst %,$(PREFIX)/tools/%.efi,$(TOOL_NAMES))

tools: all $(TOOL_EFIS) $(PREFIX)/tools/crashtest.efi $(PREFIX)/drivers/crashhandler.efi
	@echo "  Built $(words $(TOOL_NAMES)) tools + crashtest + crashhandler driver"

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
# Automatic dependency tracking (generated by -MD -MF)
# ===================================================================

-include $(wildcard $(BUILDDIR)/*.d)

clean:
	rm -rf $(PREFIX)

# Targeted clean for tool binaries (uefi-devkit references this from
# its `tools-clean` recipe — it doesn't want to wipe libaxl.a). Also
# drops the per-tool .o files since BUILD_TOOL puts them in $(BUILDDIR).
clean-tools:
	rm -f $(PREFIX)/tools/*.efi $(PREFIX)/tools/*.so
	@for t in $(TOOL_NAMES); do rm -f $(BUILDDIR)/$$t.o; done
	rm -f $(BUSYBOX_EFI) $(PREFIX)/axl.so
	rm -rf $(BUSYBOX_DIR)
