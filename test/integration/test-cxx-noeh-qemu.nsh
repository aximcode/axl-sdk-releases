@echo -off
# test-cxx-noeh-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo NOEH_BEGIN
echo --CASE-PLAIN--
plain-noeh.efi
echo --CASE-EH--
throw-eh.efi
echo --CASE-NOEH--
throw-noeh.efi
echo NOEH_DONE
