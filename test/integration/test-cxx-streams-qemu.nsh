@echo -off
# test-cxx-streams-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo STREAMS_BEGIN
cxx-streams-selftest.efi out
cxx-streams-selftest.efi str
cxx-streams-selftest.efi edge
cxx-streams-selftest.efi in < in.txt
cxx-streams-selftest.efi getline < lines.txt
cxx-streams-selftest.efi fail < bad.txt
# The shell's DEFAULT pipe, which transcodes to UCS-2. No `|a` here on
# purpose: reading raw axl_stdin would yield 4, not 42, and that is the
# whole reason axl::cin goes through axl_stdin_text().
echo 42 | cxx-streams-selftest.efi pipe
echo STREAMS_DONE
