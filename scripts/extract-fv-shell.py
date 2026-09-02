#!/usr/bin/env python3
# axl-desc: extract the UEFI Shell from a firmware volume image
# extract-fv-shell.py -- pull the UEFI Shell PE32 out of an OVMF/AAVMF
# firmware image, with zero external dependencies (Python stdlib only).
#
# Why this exists: run-qemu.sh needs a Shell.efi on the ESP so the guest
# boots a shell and runs startup.nsh. When the host has no EDK2 build, no
# distro Shell package, and no `uefiextract` (e.g. a stock Ubuntu / WSL /
# CI box -- Ubuntu's `ovmf` ships no standalone Shell, and uefiextract is
# not an apt package), nothing could stage a shell, the disk boot failed,
# and a guest NIC's PXE attempts timed out before any fallback ran. The
# shell, however, already lives inside every OVMF/AAVMF DXE firmware
# volume; this parser walks the EDK2 FV/FFS/section tree (decompressing
# the LZMA-wrapped DXE volume) and writes out the Shell PE32 -- the same
# bytes `uefiextract <fd> <shell-guid>` would yield, but with no tool to
# install.
#
# Scope: handles the encapsulation OVMF/AAVMF actually use -- nested
# firmware volumes, LZMA GUIDED sections, and uncompressed/COMPRESSION
# (type none) section streams. Tiano/UEFI-compressed and other GUIDED
# codecs are not decoded here; for such firmware the caller falls back to
# uefiextract. Exit 0 + output written on success, non-zero otherwise.

from __future__ import annotations

import argparse
import lzma
import struct
import sys
import uuid
from pathlib import Path
from typing import Iterator

# axl_version.py is staged beside this script. These tools are run by
# absolute path from the `axl` dispatcher rather than imported as a
# package, so their own directory is not already on sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from axl_version import version_string  # noqa: E402

# EFI Shell file GUID (gUefiShellFileGuid) -- same across EDK2 builds.
SHELL_GUID = uuid.UUID("7C04A583-9E3E-4F1C-AD65-E05268D0B4D1")
# LZMA_CUSTOM_DECOMPRESS_GUID -- GUIDED-section codec OVMF uses for DXEFV.
LZMA_GUID = uuid.UUID("EE4E5898-3914-4259-9D6E-DC7BD79403CF")

FVH_SIGNATURE = b"_FVH"  # at offset 40 of EFI_FIRMWARE_VOLUME_HEADER

# EFI_SECTION types we care about.
SECTION_COMPRESSION = 0x01
SECTION_GUID_DEFINED = 0x02
SECTION_PE32 = 0x10
SECTION_TE = 0x12
SECTION_FIRMWARE_VOLUME_IMAGE = 0x17

# Attribute bits.
FFS_ATTRIB_LARGE_FILE = 0x01
GUIDED_PROCESSING_REQUIRED = 0x01
FFS_FILETYPE_PAD = 0xF0


def _align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def _lzma_decompress(payload: bytes) -> bytes | None:
    # EDK2 GUIDED-LZMA payload is the standard LZMA "alone" stream:
    # 5 props bytes + 8-byte uncompressed size + data -- i.e. FORMAT_ALONE.
    try:
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(payload)
    except (lzma.LZMAError, EOFError, ValueError):
        return None


def _iter_ffs_files(fv: bytes) -> Iterator[tuple[uuid.UUID, int, bytes]]:
    # Yield (file_guid, file_type, file_data) for each real FFS file in an FV.
    if len(fv) < 0x40 or fv[40:44] != FVH_SIGNATURE:
        return
    header_len = struct.unpack_from("<H", fv, 48)[0]
    ext_header_off = struct.unpack_from("<H", fv, 52)[0]
    if ext_header_off:
        # EFI_FIRMWARE_VOLUME_EXT_HEADER: FvName[16] + ExtHeaderSize[4].
        ext_size = struct.unpack_from("<I", fv, ext_header_off + 16)[0]
        start = ext_header_off + ext_size
    else:
        start = header_len
    pos = _align(start, 8)
    while pos + 24 <= len(fv):
        name = fv[pos : pos + 16]
        file_type = fv[pos + 18]
        attrib = fv[pos + 19]
        size24 = fv[pos + 20] | (fv[pos + 21] << 8) | (fv[pos + 22] << 16)
        if size24 == 0xFFFFFF and not (attrib & FFS_ATTRIB_LARGE_FILE):
            break  # erased free space -- end of files
        header = 24
        file_size = size24
        if attrib & FFS_ATTRIB_LARGE_FILE:
            file_size = struct.unpack_from("<Q", fv, pos + 24)[0]
            header = 32
        if file_size < header or pos + file_size > len(fv):
            break
        if file_type != FFS_FILETYPE_PAD and name != b"\xff" * 16:
            yield uuid.UUID(bytes_le=name), file_type, fv[pos + header : pos + file_size]
        pos = _align(pos + file_size, 8)


def _iter_sections(blob: bytes) -> Iterator[tuple[int, bytes, int]]:
    # Yield (section_type, whole_section_bytes, body_offset) for a section stream.
    pos = 0
    while pos + 4 <= len(blob):
        size24 = blob[pos] | (blob[pos + 1] << 8) | (blob[pos + 2] << 16)
        section_type = blob[pos + 3]
        if size24 == 0xFFFFFF:
            size = struct.unpack_from("<I", blob, pos + 4)[0]
            body_off = 8
        else:
            size = size24
            body_off = 4
        if size < body_off or pos + size > len(blob):
            break
        yield section_type, blob[pos : pos + size], body_off
        pos = _align(pos + size, 4)


def _search_sections(blob: bytes, enclosing_guid: uuid.UUID) -> bytes | None:
    for section_type, section, body_off in _iter_sections(blob):
        if section_type in (SECTION_PE32, SECTION_TE):
            if enclosing_guid == SHELL_GUID:
                return section[body_off:]
        elif section_type == SECTION_GUID_DEFINED:
            codec = uuid.UUID(bytes_le=section[4:20])
            data_off = struct.unpack_from("<H", section, 20)[0]
            attrs = struct.unpack_from("<H", section, 22)[0]
            payload = section[data_off:]
            decoded = _lzma_decompress(payload) if codec == LZMA_GUID else None
            if decoded is None and not (attrs & GUIDED_PROCESSING_REQUIRED):
                decoded = payload  # processing not required: data is raw sections
            if decoded is not None:
                found = _search_sections(decoded, enclosing_guid)
                if found is not None:
                    return found
        elif section_type == SECTION_COMPRESSION:
            comp_type = section[8]
            if comp_type == 0:  # not compressed: body is a raw section stream
                found = _search_sections(section[9:], enclosing_guid)
                if found is not None:
                    return found
            # comp_type 1 (EFI/Tiano) is not handled here -- caller falls back.
        elif section_type == SECTION_FIRMWARE_VOLUME_IMAGE:
            found = _search_fv(section[body_off:])
            if found is not None:
                return found
    return None


def _search_fv(fv: bytes) -> bytes | None:
    for file_guid, _file_type, file_data in _iter_ffs_files(fv):
        found = _search_sections(file_data, file_guid)
        if found is not None:
            return found
    return None


def _iter_top_level_fvs(data: bytes) -> Iterator[bytes]:
    # A .fd image is one or more concatenated firmware volumes; locate each
    # by its _FVH signature and hand the slice to the recursive search.
    pos = 0
    while pos + 0x40 <= len(data):
        if data[pos + 40 : pos + 44] == FVH_SIGNATURE:
            fv_len = struct.unpack_from("<Q", data, pos + 32)[0]
            header_len = struct.unpack_from("<H", data, pos + 48)[0]
            if header_len >= 0x48 and fv_len >= header_len and pos + fv_len <= len(data):
                yield data[pos : pos + fv_len]
                pos = _align(pos + fv_len, 8)
                continue
        pos += 16


def extract_shell(firmware: Path) -> bytes | None:
    data = firmware.read_bytes()
    for fv in _iter_top_level_fvs(data):
        found = _search_fv(fv)
        if found is not None and found[:2] == b"MZ":
            return found
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version", action="version",
        version=f"extract-fv-shell {version_string()}")
    parser.add_argument("firmware", type=Path, help="OVMF/AAVMF firmware .fd image")
    parser.add_argument("-o", "--output", type=Path, required=True, help="write Shell.efi here")
    args = parser.parse_args()

    if not args.firmware.is_file():
        print(f"error: firmware not found: {args.firmware}", file=sys.stderr)
        return 2

    shell = extract_shell(args.firmware)
    if shell is None:
        print(f"error: no Shell PE32 found in {args.firmware}", file=sys.stderr)
        return 1

    args.output.write_bytes(shell)
    return 0


if __name__ == "__main__":
    sys.exit(main())
