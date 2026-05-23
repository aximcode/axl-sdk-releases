# Build shared-driver-demo against an installed axl-sdk.
#
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/axl-sdk-install
#   cmake --build .
#
# Produces two .efi files:
#   shared-driver-demo-dxe.efi   resident driver image
#   shared-driver-demo.efi       launcher (with the driver bytes
#                                 embedded — single-binary
#                                 distribution).
#
# To run the demo under QEMU, copy shared-driver-demo.efi to a FAT
# image alongside pci-ids.json5 (the SDK includes a copy under
# share/) and boot the image; pass `shared-driver-demo.efi run` at
# the shell. The driver image is NOT shipped separately — it's
# baked into the launcher.

cmake_minimum_required(VERSION 3.16)
project(shared-driver-demo C)

find_package(axl REQUIRED)

# Shared TU. Listed in BOTH the driver's and launcher's source lists
# below — each image compiles + links its own copy of the symbols
# defined here. This is the canonical shape for a non-trivial consumer
# that has helper functions both halves of the application use.
#
# axl-cc enforces `ld --no-undefined`, so omitting this file from
# either source list while still calling its functions surfaces as
# a precise "undefined reference to `demo_print_banner'" build error
# rather than a runtime crash on a corrupted call target.
set(DEMO_SHARED_SOURCES shared-driver-demo-format.c)

axl_add_driver(shared-driver-demo-dxe
    shared-driver-demo-driver.c
    ${DEMO_SHARED_SOURCES}
)

axl_add_app(shared-driver-demo
    shared-driver-demo-launcher.c
    ${DEMO_SHARED_SOURCES}
    EMBEDS ${shared-driver-demo-dxe_EFI_PATH}=shared_driver_demo
)
# The EMBEDS clause emits a file-DEPENDS on the driver's .efi, so
# correctness is guaranteed even without this line. But under
# parallel `make -j`, the two custom-target trees race to build
# the driver's intermediates independently — visible as duplicated
# "gcc/ld/objcopy" log lines and double the work. add_dependencies
# pins the target ordering and lets make serialize cleanly.
add_dependencies(shared-driver-demo shared-driver-demo-dxe)
