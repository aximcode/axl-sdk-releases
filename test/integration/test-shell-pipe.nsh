@echo -off
# test-shell-pipe.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).

# Shell built-ins write \r\n line endings; hexdump should show
# 'h e l l o \r \n' for `echo hello`. Each PROBE-N marker frames
# the section so we can assert on the cleaned log.
echo PROBE-1-START
echo hello | hexdump.efi
echo PROBE-1-END

echo PROBE-2-START
echo line-with-needle | grep.efi needle
echo PROBE-2-END

echo PROBE-3-START
echo nomatch | grep.efi needle
echo PROBE-3-END

# Pipe a multi-line shell-builtin output through grep. ver
# produces a couple of lines of UEFI version info; grep for a
# token that should always be present.
echo PROBE-4-START
ver | grep.efi UEFI
echo PROBE-4-END

echo === DONE ===
