@echo -off
# test-axl-busybox.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo BUSYBOX-HELP-START
axl.efi --help
echo BUSYBOX-HELP-END
echo BUSYBOX-CAT-START
axl.efi cat testdata.txt
echo BUSYBOX-CAT-END
echo BUSYBOX-GREP-START
axl.efi grep needle match.txt
echo BUSYBOX-GREP-END
echo BUSYBOX-UNKNOWN-START
axl.efi nope
echo BUSYBOX-UNKNOWN-END
