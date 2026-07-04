# Build shared-driver-demo against an installed axl-sdk.
#
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/axl-sdk-install
#   cmake --build .
#
# Produces two .efi files:
#   shared-driver-demo-dxe.efi   resident driver image (AXL_SHARED_DRIVER)
#   shared-driver-demo.efi       launcher (AXL_SHARED_DRIVER_LAUNCHER, with
#                                 the driver bytes embedded — single-binary
#                                 distribution).
#
# To run the demo under QEMU, copy shared-driver-demo.efi to a FAT image
# and boot it; pass a verb at the shell, e.g.:
#   shared-driver-demo.efi status
#   echo hi | shared-driver-demo.efi echotext
#   shared-driver-demo.efi stderr 2> err.txt
# The driver image is NOT shipped separately — it's baked into the launcher.

cmake_minimum_required(VERSION 3.16)
project(shared-driver-demo C)

find_package(axl REQUIRED)

# Driver-only helper module (shared-driver-demo-format.c). In the earlier
# custom-vtable version of this example this module was compiled into BOTH
# the driver's and launcher's source lists — a real multi-TU consumer often
# shares formatters/argv helpers across both halves; see
# AXL-Shared-Driver-Recipe.md's "Sharing helpers between launcher and
# driver" for that general pattern. The turnkey launcher below has no
# custom code left to call a shared helper from — its entire int main is
# the one AXL_SHARED_DRIVER_LAUNCHER macro invocation — so this module now
# lives only in the driver's source list.
set(DEMO_DRIVER_SOURCES shared-driver-demo-format.c)

axl_add_driver(shared-driver-demo-dxe
    shared-driver-demo-driver.c
    ${DEMO_DRIVER_SOURCES}
)

axl_add_app(shared-driver-demo
    shared-driver-demo-launcher.c
    EMBEDS ${shared-driver-demo-dxe_EFI_PATH}=shared_driver_demo
)
# The EMBEDS clause emits a file-DEPENDS on the driver's .efi, so
# correctness is guaranteed even without this line. But under
# parallel `make -j`, the two custom-target trees race to build
# the driver's intermediates independently — visible as duplicated
# "gcc/ld/objcopy" log lines and double the work. add_dependencies
# pins the target ordering and lets make serialize cleanly.
add_dependencies(shared-driver-demo shared-driver-demo-dxe)
