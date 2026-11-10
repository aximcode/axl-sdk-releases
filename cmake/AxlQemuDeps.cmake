# AxlQemuDeps.cmake -- the QEMU stack the host tools drive, per package
# family, in ONE place: the host-tools README's install hints and the
# .deb/.rpm packages' weak dependencies (AXL-Distribution-Design.md §12.6,
# §17.5) read these lists.
include_guard(GLOBAL)
set(AXL_HOST_DEPS_DEB "qemu-system-x86 qemu-system-arm ovmf qemu-efi-aarch64 virtiofsd mtools dosfstools")
set(AXL_HOST_DEPS_DNF "qemu-system-x86 qemu-system-aarch64 edk2-ovmf edk2-aarch64 virtiofsd mtools dosfstools")
set(AXL_HOST_DEPS_PACMAN "qemu-system-x86 qemu-system-aarch64 edk2-ovmf edk2-armvirt virtiofsd mtools dosfstools")
