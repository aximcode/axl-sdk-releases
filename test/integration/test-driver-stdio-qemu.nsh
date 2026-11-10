@echo -off
# test-driver-stdio-qemu.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).
# STALE-BRIDGE / WARM-PATH USE-AFTER-FREE REGRESSION (consumer do.efi):
#   1. load the driver resident via a NON-stdin verb (clean launcher),
#   2. a leaker launcher installs the bridge with a PIPE StdIn then exits
#      WITHOUT uninstalling -> a STALE bridge whose stdin_h is a freed
#      pipe handle,
#   3. a warm self-locating launcher reads a pipe. The driver must SKIP
#      the stale (dead-launcher) bridge and read THIS launcher's input
#      ("warmpipe") instead of dereferencing the freed handle (#GP/#PF
#      in Shell.dll -- the exact do.efi symptom).
# Runs FIRST so the stale instance is the OLDEST (returned first by a
# naive LocateProtocol) -- the precise ordering that crashed do.efi.
echo STALE_BEGIN
stdio-bridge-fix.efi emit
echo poison |a stdio-bridge-leak.efi
echo warmpipe |a stdio-bridge-self.efi echo
echo STALE_DONE
echo PIPE_BEGIN
# `|a` is the UEFI shell's ASCII pipe: it feeds the LHS output to the
# RHS StdIn as raw 8-bit bytes. The default `|` pipes UCS-2 (UTF-16LE),
# which axl_stdin (raw bytes) would read as 'h\0e\0...' -> GOT:h.
echo hello |a stdio-bridge-fix.efi echo
echo TEXTPIPE_BEGIN
# Default `|` pipes UCS-2; the echotext verb reads via axl_stdin_text(),
# which decodes it to UTF-8 -- so this works WITHOUT the `|a` operator.
echo textpipeinput | stdio-bridge-fix.efi echotext
echo REDIR_BEGIN
stdio-bridge-fix.efi echo < in.txt
echo EMIT_BEGIN
stdio-bridge-fix.efi emit > out.txt
echo TYPE_BEGIN
type out.txt
echo NOINPUT_BEGIN
# Empty redirected file -> clean EOF (a file is never interactive, so the
# console line-reader fallback does not engage). A non-redirected read at
# an interactive console now blocks for a typed line instead -- that path
# is covered by test-console-readline-qemu.sh.
stdio-bridge-fix.efi echo < empty.txt
# Self-locating launcher: resolves the resident driver itself (warm
# fast-path) and installs the bridge via the public escape hatch
# axl_shared_driver_install_stdio_bridge(). The driver is resident
# because the locate launcher above already loaded it. Distinct input
# values keep these GOT: tokens unambiguous from the locate cases.
echo SELF_BEGIN
echo selfhello |a stdio-bridge-self.efi echo
echo selftext | stdio-bridge-self.efi echotext
stdio-bridge-self.efi echo < in2.txt
echo SELF_DONE
# A launcher built before the bridge carried a target: its bridge is
# under the original GUID only, and the 8 bytes past it hold this
# driver's interface. The driver must not take its StdIn -- reading a
# target there is a read past the end of the launcher's static.
echo OLD_BEGIN
stdio-bridge-old.efi echo < in3.txt
echo OLD_DONE
# Raw stdout from the resident driver, one launcher per line: the
# console's decoder state (UTF-16 mode from a mark, a held partial
# character) belongs to the launcher that left it, not the next one --
# though every launcher's console handle is the same Shell file.
echo RAWKEY_BEGIN
stdio-bridge-fix.efi rawbom
stdio-bridge-fix.efi rawplain
stdio-bridge-fix.efi rawsplit
stdio-bridge-fix.efi rawplain
echo RAWKEY_DONE
echo ESTAT_BEGIN
stdio-bridge-self.efi exitstatus
echo ESTAT=%lasterror%
echo ESTAT_DONE
echo STDIO_DONE
