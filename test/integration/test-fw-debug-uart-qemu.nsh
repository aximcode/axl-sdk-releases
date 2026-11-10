@echo -off
# test-fw-debug-uart-qemu.sh's commands: an app returning EFI_COMPROMISED_DATA,
# which a DEBUG firmware's DxeCore reports ("Error: Image at ... start failed").
echo FWDBG_BEGIN
exit-status-selftest.efi 8000000000000021
echo FWDBG_END
