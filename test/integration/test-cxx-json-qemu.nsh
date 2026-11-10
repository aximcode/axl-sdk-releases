@echo -off
# test-cxx-json-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo JSON_BEGIN
cxx-json-selftest.efi read
cxx-json-selftest.efi chain
cxx-json-selftest.efi range
cxx-json-selftest.efi write
cxx-json-selftest.efi splice
cxx-json-selftest.efi scan
cxx-json-selftest.efi own
echo JSON_DONE
