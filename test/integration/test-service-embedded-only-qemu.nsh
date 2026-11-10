@echo -off
# test-service-embedded-only-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
svc_embonly.efi start
echo %1
