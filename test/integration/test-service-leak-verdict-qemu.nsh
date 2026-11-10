@echo -off
# test-service-leak-verdict-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
svc_verdict.efi start --detach
stall 500000
svc_verdict.efi stop
svc_verdict_leak.efi start --detach
stall 500000
svc_verdict_leak.efi stop
