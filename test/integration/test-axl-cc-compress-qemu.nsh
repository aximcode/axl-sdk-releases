@echo -off
# test-axl-cc-compress-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo HELLO_BEGIN
hello.efi world
echo HELLO_END
echo RAW_BEGIN
fetch-raw.efi
echo RAW_EXIT=%lasterror%
echo RAW_END
echo FETCH_BEGIN
fetch.efi
echo FETCH_EXIT=%lasterror%
echo FETCH_END
echo CMAKE_BEGIN
fetchc.efi
echo CMAKE_EXIT=%lasterror%
echo CMAKE_END
echo CC_DONE
