@echo -off
# test-service-startfail-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory). %1 is the marker.
# Run the failing start TWICE: each failed start buffer-loads the embedded
# driver afresh, so a per-failure leak of the synthesized device path (or an
# orphaned image handle) would compound here. Both must fail cleanly and
# RETURN -- if either wedges, the marker below never prints.
svc_startfail.efi start
svc_startfail.efi start
echo %1
