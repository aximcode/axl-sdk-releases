#!/usr/bin/env python3
"""hostkey-corrupt-seed.py -- an openssh-key-v1 ed25519 file whose seed no
longer derives its public key, and NOTHING else wrong with it.

test-sshd-hostkey-qemu.sh uses it for the case sshd's launcher cannot see:
every check a parser can make without an Ed25519 implementation -- the
magic, the cipher and KDF "none", the two checkints, the padding, the public
key in the envelope equal to the one in the private section, and the second
half of the 64-byte private key equal to it too -- still passes. Only
deriving the public key from the seed, which the driver does, finds it.

One seed byte is flipped (the first). The input is a key file as
`ssh-keygen -t ed25519 -N ''` writes it.

Usage:
    hostkey-corrupt-seed.py IN OUT
"""

from __future__ import annotations

import base64
import struct
import sys
from pathlib import Path

BEGIN = "-----BEGIN OPENSSH PRIVATE KEY-----"
END = "-----END OPENSSH PRIVATE KEY-----"
SEED_LEN = 32
PUB_LEN = 32


def read_blob(text: str) -> bytes:
    """The base64 body between the armour lines, decoded."""
    lines = [ln.strip() for ln in text.splitlines()]
    start = lines.index(BEGIN) + 1
    stop = lines.index(END)
    return base64.b64decode("".join(lines[start:stop]))


def flip_seed(blob: bytes) -> bytes:
    """Flip the seed's first byte. The private section holds the 64-byte
    key as string(seed || pub); exactly one such string must exist."""
    marker = struct.pack(">I", SEED_LEN + PUB_LEN)
    hits: list[int] = []
    at = blob.find(marker)
    while at >= 0:
        body = at + len(marker)
        key = blob[body:body + SEED_LEN + PUB_LEN]
        # The second half is the public key, which appears earlier as
        # string(pub) too: that is what tells this string from a chance
        # match.
        pub = key[SEED_LEN:]
        if len(key) == SEED_LEN + PUB_LEN and struct.pack(">I", PUB_LEN) + pub in blob[:at]:
            hits.append(body)
        at = blob.find(marker, at + 1)
    if len(hits) != 1:
        raise SystemExit(f"hostkey-corrupt-seed: {len(hits)} private keys found, not 1")
    out = bytearray(blob)
    out[hits[0]] ^= 0x01
    return bytes(out)


def write_key(path: Path, blob: bytes) -> None:
    """Armour as ssh-keygen does: 70 base64 characters a line."""
    b64 = base64.b64encode(blob).decode("ascii")
    body = "\n".join(b64[i:i + 70] for i in range(0, len(b64), 70))
    path.write_text(f"{BEGIN}\n{body}\n{END}\n")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    blob = read_blob(Path(argv[1]).read_text())
    write_key(Path(argv[2]), flip_seed(blob))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
