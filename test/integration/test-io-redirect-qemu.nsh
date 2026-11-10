@echo -off
# test-io-redirect-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
echo R_BEGIN
io-streams.efi > out.txt 2> err.txt
echo TYPE_OUT_BEGIN
type out.txt
echo TYPE_OUT_END
echo TYPE_ERR_BEGIN
type err.txt
echo TYPE_ERR_END
# The err.txt (and out.txt) files are a mix of UCS-2 console text
# (from ConOut/ConErr's CHAR16 WriteFile) and raw single bytes (from
# axl_stdout_raw/axl_stderr_raw's direct WriteFile) -- 'type' decodes
# the whole file as UCS-2, so raw ASCII bytes come out as garbled
# multi-byte glyphs (each 2 raw bytes fused into one CJK-ish code
# point). hexdump shows wire truth instead: dump both files and
# look at the ASCII column for the raw marker (see PROBE 1 in
# test-shell-pipe.sh for the same "hexdump shows wire truth"
# precedent with axl_stdin).
echo HEX_OUT_BEGIN
hexdump.efi out.txt
echo HEX_OUT_END
echo HEX_ERR_BEGIN
hexdump.efi err.txt
echo HEX_ERR_END
# One stream redirected, the other on the console. Standard error on the
# console goes through the console's own ConOut splitter, never through
# gST->ConOut: `>` swaps gST->ConOut for the file, so a stderr line written
# there would land in it. The raw stderr bytes go to the console too.
echo ONLY_OUT_BEGIN
io-streams.efi > out1.txt
echo ONLY_OUT_END
echo TYPE_OUT1_BEGIN
type out1.txt
echo TYPE_OUT1_END
echo ONLY_ERR_BEGIN
io-streams.efi text 2> err2.txt
echo ONLY_ERR_END
echo TYPE_ERR2_BEGIN
type err2.txt
echo TYPE_ERR2_END
# The Shell's other sinks: NUL and an environment variable. Neither has a
# file size, as the console pseudo-files have none, so "no size" cannot
# mean "console" -- a stream is the console only when it IS the Shell's
# console file.
echo NUL_ERR_BEGIN
io-streams.efi text 2> NUL
echo NUL_ERR_END
echo NUL_OUT_BEGIN
io-streams.efi text > NUL
echo NUL_OUT_END
echo ENV_ERR_BEGIN
io-streams.efi text 2>v ioe
echo ENV_ERR_END
echo ENV_ERR_VAL_BEGIN
echo %ioe%
echo ENV_ERR_VAL_END
echo ENV_OUT_BEGIN
io-streams.efi text >v ioo
echo ENV_OUT_END
echo ENV_OUT_VAL_BEGIN
echo %ioo%
echo ENV_OUT_VAL_END
# Raw bytes to the console, both streams: exactly the bytes, nothing past
# them (io-streams.efi takes them from a buffer with no NUL after them).
echo RAWCON_BEGIN
io-streams.efi raw
echo RAWCON_END
# UTF-16LE in two raw writes, the mark in the first only: one line, abcd.
echo RAWSPLIT_BEGIN
io-streams.efi split
echo RAWSPLIT_END
echo R_DONE
