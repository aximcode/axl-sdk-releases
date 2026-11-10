@echo -off
# test-harness-exception-qemu.sh's commands: a deliberate #UD.
echo EXC_BEGIN
CrashTest.efi ud
echo EXC_NOT_REACHED
