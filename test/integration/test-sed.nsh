@echo -off
# test-sed.sh's commands, run from the directory holding its images
# (booted: fs0:\, pooled: the lease's share directory).

# s/// at end-of-line -- zero-width match (the heap-underflow regression).
echo === TEST-DOLLAR-START ===
echo a | sed.efi "s/$/X/"
echo === TEST-DOLLAR-END ===
# global substitution.
echo === TEST-SUBG-START ===
echo hello | sed.efi "s/l/L/g"
echo === TEST-SUBG-END ===
# BRE default + the \xHH (space) escape, greedy head strip.
echo === TEST-BREHEAD-START ===
echo Tag: HELLO | sed.efi "s/.*:\x20//"
echo === TEST-BREHEAD-END ===
# BRE capture group with the GNU \( \) \+ forms.
echo === TEST-BREGRP-START ===
echo foobar | sed.efi "s/\(o\+\)/[\1]/"
echo === TEST-BREGRP-END ===
# ERE capture group (-E) -- the bare ( ) + forms.
echo === TEST-EREGRP-START ===
echo foobar | sed.efi -E "s/(o+)/[\1]/"
echo === TEST-EREGRP-END ===
# y/// transliteration.
echo === TEST-Y-START ===
echo abcabc | sed.efi "y/abc/xyz/"
echo === TEST-Y-END ===
# Nth-occurrence substitution.
echo === TEST-NTH-START ===
echo abcabc | sed.efi "s/a/X/2"
echo === TEST-NTH-END ===
# case-insensitive flag.
echo === TEST-ICASE-START ===
echo HELLO | sed.efi "s/hello/Q/I"
echo === TEST-ICASE-END ===
# -n with a print address.
echo === TEST-PRINT2-START ===
sed.efi -n "2p" nums.txt
echo === TEST-PRINT2-END ===
# numeric range.
echo === TEST-RANGE-START ===
sed.efi -n "2,3p" nums.txt
echo === TEST-RANGE-END ===
# N join + substitute the embedded newline.
echo === TEST-NJOIN-START ===
sed.efi "N;s/\n/-/" pairs.txt
echo === TEST-NJOIN-END ===
# hold space: stash line 1, append it after line 2.
echo === TEST-HOLD-START ===
sed.efi -n "1h;2{G;p}" pairs.txt
echo === TEST-HOLD-END ===
# -s: per-file last line.
echo === TEST-SEP-START ===
sed.efi -s -n "$p" f1.txt f2.txt
echo === TEST-SEP-END ===
# append a line after.
echo === TEST-APPEND-START ===
echo x | sed.efi "a APP_SENTINEL"
echo === TEST-APPEND-END ===
# error path: bare line address 0 is rejected.
echo === TEST-ZEROERR-START ===
echo x | sed.efi "0d"
echo === TEST-ZEROERR-END ===
# -z NUL-delimited INPUT: three NUL-separated records a/b/c; -zn 'l'
# prints each record unambiguously with a trailing '$', one per line.
# If -z input is broken (reads \n instead), the whole file is one record
# and 'l' would print the NUL bytes as \000 on a single line.
echo === TEST-ZNULINPUT-START ===
sed.efi -zn l nulrecs.bin
echo === TEST-ZNULINPUT-END ===

