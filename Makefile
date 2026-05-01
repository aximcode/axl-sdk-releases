# Makefile — Build AXL library and tests (GCC toolchain)
#
# Usage:
#   make                          # build libaxl.a (x64)
#   make ARCH=aa64                # cross-compile for aarch64
#   make tests                    # build all test EFIs
#   make tools                    # build standalone tool EFIs
#   make clean

ARCH       ?= x64
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
  GCC_ARCH   = -mno-outline-atomics
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

# Common flags for the EFI link step. The .so we produce here is just
# an intermediate consumed by objcopy → PE/COFF; no OS ever loads it,
# so the linker's RWX-segment warning is a false positive (the resulting
# .efi has properly split per-section permissions, see PE characteristics).
LDFLAGS_EFI = -nostdlib -shared -Bsymbolic --no-warn-rwx-segments

CFLAGS_BASE = -ffreestanding -fshort-wchar \
              -fno-stack-protector -fno-builtin \
              -fno-omit-frame-pointer \
              -fpic $(GCC_ARCH) \
              -Wall \
              -DAXL_BACKEND_NATIVE

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

# ===================================================================
# Common configuration
# ===================================================================

CFLAGS     = $(CFLAGS_BASE) $(CFLAGS_BUILD) -MD
INCLUDES   = -Iinclude -Isrc/backend

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
    src/mem/axl-mem.c \
    src/mem/axl-intrinsics.c \
    src/mem/axl-arena.c \
    src/format/axl-format.c \
    src/log/axl-log.c \
    src/log/axl-log-ring.c \
    src/log/axl-log-file.c \
    src/data/axl-str.c \
    src/data/axl-str-compat.c \
    src/data/axl-string.c \
    src/data/axl-str-wide.c \
    src/data/axl-hash-table.c \
    src/data/axl-array.c \
    src/data/axl-list.c \
    src/data/axl-slist.c \
    src/data/axl-queue.c \
    src/data/axl-json-parse.c \
    src/data/axl-json5-parse.c \
    src/data/axl-json-build.c \
    src/data/axl-json-print.c \
    src/data/axl-cache.c \
    src/data/axl-radix-tree.c \
    src/data/axl-ring-buf.c \
    src/data/axl-digest.c \
    src/data/axl-digest-md5.c \
    src/data/axl-digest-sha1.c \
    src/data/axl-digest-sha256.c \
    src/io/axl-io.c \
    src/io/axl-io-buf.c \
    src/io/axl-io-file.c \
    src/util/axl-path.c \
    src/util/axl-hexdump.c \
    src/util/axl-time.c \
    src/util/axl-env.c \
    src/util/axl-sys.c \
    src/util/axl-nvstore.c \
    src/util/axl-io-port.c \
    src/util/axl-boot.c \
    src/util/axl-image.c \
    src/util/axl-mem-phys.c \
    src/util/axl-watchdog.c \
    src/util/axl-rng.c \
    src/util/axl-service.c \
    src/util/axl-driver.c \
    src/util/axl-diag.c \
    src/util/axl-config.c \
    src/util/axl-subcommand.c \
    src/util/axl-args.c \
    src/smbios/axl-smbios.c \
    src/acpi/axl-acpi.c \
    src/pci/axl-pci.c \
    src/loop/axl-loop.c \
    src/loop/axl-defer.c \
    src/loop/axl-pubsub.c \
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
    src/net/axl-net-resolve.c \
    src/net/axl-net-interfaces.c \
    src/net/axl-net-addr.c \
    src/net/axl-net-dhcp.c \
    src/net/axl-http-core.c \
    src/net/axl-http-server.c \
    src/net/axl-http-route.c \
    src/net/axl-http-conn.c \
    src/net/axl-http-request.c \
    src/net/axl-http-dispatch.c \
    src/net/axl-http-response.c \
    src/net/axl-http-upload.c \
    src/net/axl-http-ws.c \
    src/net/axl-http-client.c \
    src/net/axl-tls.c \
    src/net/axl-url.c \
    src/net/axl-inet-address.c \
    src/net/axl-socket.c \
    src/net/axl-socket-client.c \
    src/net/axl-websocket.c \
    src/gfx/axl-gfx.c \
    src/smbus/axl-smbus.c \
    src/smbus/axl-smbus-hc.c \
    src/smbus/axl-smbus-i2c.c \
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
    src/posix/axl-app.c \
    src/runtime/axl-atexit.c \
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
    deps/mbedtls/library/hkdf.c \
    deps/mbedtls/library/md.c \
    deps/mbedtls/library/oid.c \
    deps/mbedtls/library/pem.c \
    deps/mbedtls/library/pk.c \
    deps/mbedtls/library/pkparse.c \
    deps/mbedtls/library/pkwrite.c \
    deps/mbedtls/library/pk_ecc.c \
    deps/mbedtls/library/pk_wrap.c \
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

# CRT0 objects (C entry point bridges).
#   native  -- full runtime: registry, atexit, signal notify, default loop.
#   minimal -- opt-out variant for size-constrained or exit-managed apps.
# Both are built unconditionally; axl-cc picks one at link time.
CRT0_OBJ         = $(BUILDDIR)/axl-crt0-native.o
CRT0_MINIMAL_OBJ = $(BUILDDIR)/axl-crt0-minimal.o

# ===================================================================
# Default target
# ===================================================================

.PHONY: all clean hello gfx-demo driver smbus-hc-shim radix-demo ring-buf-demo event-demo cancellable-demo runtime-demo echo-server tcp-echo-server echo-client echo-server-sync kernel-poc axlk-echo-server axlk-hwinfo-server axlk-bootconfig-server axlk-reqlog-server tests tools check-version

# Pin the default goal so rule order can't turn check-version (or
# any future helper target) into the default by accident.
.DEFAULT_GOAL := all

all: check-version $(PREFIX)/lib/libaxl.a $(GCC_CRT0) $(RELOC_OBJ) $(DEBUG_INFO_OBJ) $(CRT0_OBJ) $(CRT0_MINIMAL_OBJ) $(PE_SET_DEBUG)
	@echo ""
	@echo "  AXL library built (gcc, $(ARCH))"
	@echo "  Library:  $(PREFIX)/lib/libaxl.a"
	@echo "  Headers:  include/axl.h"
	@echo ""

# Verify VERSION file and include/axl/axl-version.h agree. Use
# scripts/bump-version.sh to update both atomically.
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

$(BUILDDIR)/%.o: src/io/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/util/%.c | $(BUILDDIR)
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

$(BUILDDIR)/%.o: src/smbios/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/acpi/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILDDIR)/%.o: src/pci/%.c | $(BUILDDIR)
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

$(PREFIX)/lib/libaxl.a: $(LIB_OBJS) | $(PREFIX)/lib
	$(AR) rcs $@ $^

$(BUILDDIR):
	mkdir -p $@

$(PREFIX)/lib:
	mkdir -p $@

# ===================================================================
# Build hello.efi example
# ===================================================================

hello: $(PREFIX)/hello.efi
	@echo "  Built: $(PREFIX)/hello.efi"

$(PREFIX)/hello.efi: $(BUILDDIR)/hello.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/hello.o,$@)

$(BUILDDIR)/hello.o: sdk/examples/hello.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build gfx-demo.efi example
# ===================================================================

gfx-demo: $(PREFIX)/gfx-demo.efi
	@echo "  Built: $(PREFIX)/gfx-demo.efi"

$(PREFIX)/gfx-demo.efi: $(BUILDDIR)/gfx-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/gfx-demo.o,$@)

$(BUILDDIR)/gfx-demo.o: sdk/examples/gfx-demo.c | $(BUILDDIR)
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
# Build radix-demo.efi example
# ===================================================================

radix-demo: $(PREFIX)/radix-demo.efi
	@echo "  Built: $(PREFIX)/radix-demo.efi"

$(PREFIX)/radix-demo.efi: $(BUILDDIR)/radix-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/radix-demo.o,$@)

$(BUILDDIR)/radix-demo.o: sdk/examples/radix-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build ring-buf-demo.efi example
# ===================================================================

ring-buf-demo: $(PREFIX)/ring-buf-demo.efi
	@echo "  Built: $(PREFIX)/ring-buf-demo.efi"

$(PREFIX)/ring-buf-demo.efi: $(BUILDDIR)/ring-buf-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/ring-buf-demo.o,$@)

$(BUILDDIR)/ring-buf-demo.o: sdk/examples/ring-buf-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build event-demo.efi example
# ===================================================================

event-demo: $(PREFIX)/event-demo.efi
	@echo "  Built: $(PREFIX)/event-demo.efi"

$(PREFIX)/event-demo.efi: $(BUILDDIR)/event-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/event-demo.o,$@)

$(BUILDDIR)/event-demo.o: sdk/examples/event-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build cancellable-demo.efi example
# ===================================================================

cancellable-demo: $(PREFIX)/cancellable-demo.efi
	@echo "  Built: $(PREFIX)/cancellable-demo.efi"

$(PREFIX)/cancellable-demo.efi: $(BUILDDIR)/cancellable-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/cancellable-demo.o,$@)

$(BUILDDIR)/cancellable-demo.o: sdk/examples/cancellable-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build runtime-demo.efi example (Phase A7 prototype)
# ===================================================================

runtime-demo: $(PREFIX)/runtime-demo.efi
	@echo "  Built: $(PREFIX)/runtime-demo.efi"

$(PREFIX)/runtime-demo.efi: $(BUILDDIR)/runtime-demo.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/runtime-demo.o,$@)

$(BUILDDIR)/runtime-demo.o: sdk/examples/runtime-demo.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-server.efi example (AxlSocket async echo)
# ===================================================================

echo-server: $(PREFIX)/echo-server.efi
	@echo "  Built: $(PREFIX)/echo-server.efi"

$(PREFIX)/echo-server.efi: $(BUILDDIR)/echo-server.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/echo-server.o,$@)

$(BUILDDIR)/echo-server.o: sdk/examples/echo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build tcp-echo-server.efi example (AxlTcp primitives)
# ===================================================================

tcp-echo-server: $(PREFIX)/tcp-echo-server.efi
	@echo "  Built: $(PREFIX)/tcp-echo-server.efi"

$(PREFIX)/tcp-echo-server.efi: $(BUILDDIR)/tcp-echo-server.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/tcp-echo-server.o,$@)

$(BUILDDIR)/tcp-echo-server.o: sdk/examples/tcp-echo-server.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-client.efi example (sync AxlSocket client)
# ===================================================================

echo-client: $(PREFIX)/echo-client.efi
	@echo "  Built: $(PREFIX)/echo-client.efi"

$(PREFIX)/echo-client.efi: $(BUILDDIR)/echo-client.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(BUILDDIR)/echo-client.o,$@)

$(BUILDDIR)/echo-client.o: sdk/examples/echo-client.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ===================================================================
# Build echo-server-sync.efi example (single-client sync AxlSocket)
# ===================================================================

echo-server-sync: $(PREFIX)/echo-server-sync.efi
	@echo "  Built: $(PREFIX)/echo-server-sync.efi"

$(PREFIX)/echo-server-sync.efi: $(BUILDDIR)/echo-server-sync.o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
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

$(PREFIX)/AxlKernelPoc.efi: $(KERNEL_POC_OBJS) $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(KERNEL_POC_OBJS),$@)

$(PREFIX)/axlk-echo-server.efi: $(AXLK_ECHO_OBJS) $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_ECHO_OBJS),$@)

$(PREFIX)/axlk-hwinfo-server.efi: $(AXLK_HWINFO_OBJS) $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_HWINFO_OBJS),$@)

$(PREFIX)/axlk-bootconfig-server.efi: $(AXLK_BOOTCFG_OBJS) $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
	$(call LINK_EFI_APP,$(AXLK_BOOTCFG_OBJS),$@)

$(PREFIX)/axlk-reqlog-server.efi: $(AXLK_REQLOG_OBJS) $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a
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

TEST_CFLAGS = $(CFLAGS) $(INCLUDES) -Itest/unit -Itest/data

TESTS = AxlTestMem AxlTestString AxlTestIO AxlTestLog \
        AxlTestData AxlTestUtil AxlTestLoop AxlTestTask AxlTestNet \
        AxlTestSmbus AxlTestIpmi AxlTestPlatform AxlTestEvent \
        AxlTestCpuIdle AxlTestRuntime

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

# ===================================================================
# Tools (standalone UEFI utilities)
# ===================================================================

TOOL_NAMES = hexdump fetch find grep sysinfo netinfo mkrd rfbrowse ipmi dmidecode memspd
TOOL_EFIS  = $(patsubst %,$(PREFIX)/tools/%.efi,$(TOOL_NAMES))

tools: $(TOOL_EFIS)
	@echo "  Built $(words $(TOOL_NAMES)) tools"

# Embedded driver blob for mkrd. Vendored EDK2 RamDiskDxe.efi (one per
# arch) is converted to a `static const unsigned char[]` header at build
# time; mkrd LoadImages it from memory when the host firmware doesn't
# ship EFI_RAM_DISK_PROTOCOL. See third_party/edk2/README.md for
# provenance and license.
#
# xxd -i emits non-static `unsigned char NAME[]` + `unsigned int
# NAME_len`; sed adds `const` so the data lands in .rodata. Symbols
# stay external-linkage — mkrd.c declares them via `extern const`
# at the top of the file (matching declarations satisfy editors and
# static analyzers that don't see the -include flag).
EMBEDDED_RAMDISK_SRC = third_party/edk2/RamDiskDxe-$(ARCH).efi
EMBEDDED_RAMDISK_HDR = $(BUILDDIR)/mkrd-ramdisk-blob.h

$(EMBEDDED_RAMDISK_HDR): $(EMBEDDED_RAMDISK_SRC) | $(BUILDDIR)
	@echo "  EMBED   $< -> $(notdir $@)"
	@cd $(dir $<) && xxd -i -n axl_embedded_ramdiskdxe $(notdir $<) \
	    | sed -e 's/^unsigned char/const unsigned char/' \
	          -e 's/^unsigned int/const unsigned int/' \
	    > $(abspath $@)

define BUILD_TOOL
$(PREFIX)/tools/$(1).efi: $(BUILDDIR)/$(1).o $(CRT0_OBJ) $(PREFIX)/lib/libaxl.a | $(PREFIX)/tools
	$$(call LINK_EFI_APP,$(BUILDDIR)/$(1).o,$$@)

$(BUILDDIR)/$(1).o: tools/$(1).c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $$< -o $$@
endef

$(foreach t,$(TOOL_NAMES),$(eval $(call BUILD_TOOL,$(t))))

# mkrd needs the embedded RamDiskDxe blob — override the generic
# tool rule with one that depends on (and includes) the generated
# header. -include forces the header into the translation unit
# without touching mkrd.c's #include list.
$(BUILDDIR)/mkrd.o: tools/mkrd.c $(EMBEDDED_RAMDISK_HDR) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -include $(EMBEDDED_RAMDISK_HDR) -c $< -o $@

$(PREFIX)/tools:
	@mkdir -p $@

# ===================================================================
# Clean
# ===================================================================

# ===================================================================
# Automatic dependency tracking (generated by -MD -MF)
# ===================================================================

-include $(wildcard $(BUILDDIR)/*.d)

clean:
	rm -rf $(PREFIX)
