@echo -off
# test-cxx-seam-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo SEAM_BEGIN
cxx-seam-selftest.efi cstr
cxx-seam-selftest.efi array
cxx-seam-selftest.efi ntree
cxx-seam-selftest.efi radix
cxx-seam-selftest.efi gfx
# Runs LAST: it halts, and a halt that took the shell down with it would
# otherwise cost every verb after it.
echo SEAM_MISMATCH
cxx-seam-selftest.efi mismatch
echo SEAM_DONE
