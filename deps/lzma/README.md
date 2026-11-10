# Vendored LZMA SDK (public domain)

The C/ subset of 7-Zip by Igor Pavlov (public domain). Source of the branch
filter files and the provenance check for the codec files:

    https://github.com/ip7z/7zip   tag 26.01   (C/ directory)
    https://raw.githubusercontent.com/ip7z/7zip/26.01/C/<file>

## Files added for the pack module (byte-identical to the tag, LF endings)

| file          | SHA-256 |
|---------------|---------|
| Bra.c         | c90cbc0747a406d056daf3d7b2bf05cf88c55529777469db5ca15f754b7c38e8 |
| Bra.h         | bf4addce6209562a700edf040a8efc5b392bb33d4427b1513b6e3a877d04f431 |
| Bra86.c       | 0925d655955bdd2b7b25a01bbb3d2d395e5bac6d572f2cfd273141d1896af0ec |
| RotateDefs.h  | 9bc3ff572bec5611c5c2508b7d0c18706a1dc4c908ea3bd432237c33056e1f28 |

`RotateDefs.h` is the only extra header `Bra.c` needs that was not already
here (`Bra.h` pulls `7zTypes.h`, `Bra.c`/`Bra86.c` pull `Precomp.h` and
`CpuArch.h`, all present).

## Earlier files

`LzmaDec.c` and `LzmaEnc.c` are the 26.01 files with CRLF line endings: each
is byte-identical to the tag's file after converting its LF to CRLF
(`sed 's/$/\r/'`). SHA-256 as stored here (CRLF):

| file       | SHA-256 |
|------------|---------|
| LzmaDec.c  | 0b938edae2aec133d144e2ff3493ac2b82850b3bbf8a79d1e925409124d39933 |
| LzmaEnc.c  | d7b11ae19cdc2a808ec0c11e866b6559d811a2506500f0920b3b0a2308f8a8c7 |

`errno.h` is an AXL-written shim, not upstream.
