@echo -off
# test-tools.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).

echo === TEST-HEXDUMP ===
hexdump.efi -n 32 testdata.txt

echo === TEST-GREP-MATCH ===
grep.efi Hello testdata.txt

echo === TEST-GREP-MISS ===
grep.efi ZZZNOMATCH testdata.txt

echo === TEST-GREP-RECURSIVE ===
grep.efi needle testdir

echo === TEST-GREP-LONGLINE ===
# 4096-byte line with pattern at byte 3000 -- proves grep
# streams via axl_readline rather than the previous 1024-byte
# fixed buffer that would have truncated the line below the
# match offset.
grep.efi NEEDLE grep_long.txt

echo === TEST-GREP-REGEX ===
# -E enables AxlRegex. 'a.ple' (dot = any char) matches "apple"; a
# literal "a.ple" would not -- proves the regex path is live.
grep.efi -E a.ple regex_test.txt
# -E -i: case-insensitive regex 'BAN.NA' matches "banana".
grep.efi -E -i BAN.NA regex_test.txt

echo === TEST-FIND ===
find.efi testdir

echo === TEST-SYSINFO ===
sysinfo.efi

echo === TEST-DMIDECODE-VERSION ===
dmidecode.efi --smbios-version

echo === TEST-DMIDECODE-BIOS ===
dmidecode.efi -t 0

echo === TEST-DMIDECODE-STRING ===
dmidecode.efi -s system-manufacturer

echo === TEST-DMIDECODE-UNDECODED ===
# Type 32 (System Boot Information) has no specialized decoder, so
# the tool should fall back to Header-and-Data + Strings.
dmidecode.efi -t 32

echo === TEST-CAT-PLAIN ===
cat.efi testdata.txt

echo === TEST-CAT-NUMBER-SQUEEZE ===
# -n numbers all output lines; -s collapses the run of blanks in
# cat3.txt into a single blank line. Expect 4 numbered lines:
# `     1\tfirst`, `     2\tsecond`, `     3\t` (the squeezed
# blank), `     4\tthird`.
cat.efi -n -s cat3.txt

echo === TEST-CAT-UCS2-BOM ===
# BOM-probe path: the file is UCS-2 LE with BOM; cat must transcode
# and emit "ux" as plain UTF-8.
cat.efi cat_ucs2.txt

echo === TEST-CAT-FORCE-ENCODING ===
# Force the encoding even though the file has no BOM (drop the
# first 2 bytes via -e). Use the same UCS-2 LE file to verify -e
# parses correctly and the wire-side bytes still decode to text.
cat.efi -e ucs2le cat_ucs2.txt

echo === TEST-CAT-UCS2-HEADERLESS ===
# Auto-detect UCS-2 LE without a BOM via the content sniff in
# axl_text_stream_wrap. Body is "headless" -- should appear as
# plain UTF-8 on the console.
cat.efi cat_ucs2nb.txt

echo === TEST-MKRD-HELP ===
mkrd.efi -h

echo === TEST-MKRD-POSITIONAL ===
# Reproduces the user-reported case: positional label arg.
# When RamDiskDxe.efi is staged at drivers/<arch>/, mkrd's
# axl_driver_ensure() auto-load should find it and create the
# ramdisk. When not staged, mkrd should print "not found on any
# mounted volume" -- still proof argv[1] reached the program.
mkrd.efi testrd

echo === TEST-MKRD-LIST ===
mkrd.efi -l

# --- flag-level coverage absorbed from uefi-devkit's test-shelltools.sh
#     (so that test can be retired without losing tool-flag coverage) ---
echo === TEST-GREP-ICASE-START ===
# -i: case-insensitive literal -- the uppercase "HELLO WORLD" line only
# appears under -i (a plain `grep hello` would miss it).
grep.efi -i hello grep_multi.txt
echo === TEST-GREP-ICASE-END ===

echo === TEST-GREP-LINENUM-START ===
# -n prefixes "1:" etc.
grep.efi -n hello grep_multi.txt
echo === TEST-GREP-LINENUM-END ===

echo === TEST-GREP-COUNT-START ===
# -c prints just "2"
grep.efi -c hello grep_multi.txt
echo === TEST-GREP-COUNT-END ===

echo === TEST-FIND-NAME-START ===
# Quoted so the UEFI shell doesn't wildcard-expand against cwd; find
# must apply the *.txt filter itself (match.txt in, other.log out).
find.efi --name "*.txt" testdir
echo === TEST-FIND-NAME-END ===

echo === TEST-FIND-TYPE-START ===
# dirs only: subdir in, match.txt out
find.efi --type d testdir
echo === TEST-FIND-TYPE-END ===

echo === TEST-HEXDUMP-OFFSET-START ===
# first addr column = 00000010
hexdump.efi -o 16 -n 32 testdata.txt
echo === TEST-HEXDUMP-OFFSET-END ===

echo === TEST-SYSINFO-FW-START ===
# `sysinfo fw` is firmware-only: prints "=== Firmware ===" but NOT the
# "=== CPU ===" section the bare sysinfo emits.
sysinfo.efi fw
echo === TEST-SYSINFO-FW-END ===

echo === TEST-SYSINFO-ARCH-START ===
# prints x64 / aa64
sysinfo.efi arch
echo === TEST-SYSINFO-ARCH-END ===

echo === TEST-MKRD-DESTROY-START ===
# testrd was created above (TEST-MKRD-POSITIONAL); tear it down.
mkrd.efi -v -d testrd
echo === TEST-MKRD-DESTROY-END ===

echo === TEST-END ===
