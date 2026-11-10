@echo -off
# test-driver-identity-qemu.sh's commands: run from the directory holding
# its images (booted: fs0:\, pooled: the lease's share directory).
driver-identity-test.efi
echo DH_PLAIN_BEGIN
dh -p A11CE5ED-1DE4-7E57-B00B-5FDA84D40001
echo DH_VERBOSE_BEGIN
dh -v -p A11CE5ED-1DE4-7E57-B00B-5FDA84D40001
echo DH_DONE
