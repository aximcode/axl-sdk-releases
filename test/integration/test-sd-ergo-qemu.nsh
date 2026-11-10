@echo -off
# test-sd-ergo-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo ERGO_BEGIN
echo hello | sd-ergo-launcher.efi echotext
sd-ergo-launcher.efi status
echo ESTAT=%lasterror%
echo ERGO_DONE
