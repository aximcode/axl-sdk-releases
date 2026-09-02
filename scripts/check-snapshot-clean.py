#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Refuse to publish a source snapshot that carries private infrastructure.

WHY THIS EXISTS. `aximcode/axl-sdk-releases` is public; this repo is not. Every
release pushes a squashed source snapshot there, and the selection rule was
"everything tracked, minus `.github/`" -- one exclusion, for a mechanical
reason (a PAT without `workflow` scope cannot push workflow files), not an
editorial one. So every tracked document was public, including ones written as
internal working notes, and nobody had decided that.
AXL-Distribution-Design.md §15.

IT RUNS ON THE ASSEMBLED SNAPSHOT, NOT THE REPO. That is the whole point: it
asserts the property that matters -- "what we are about to push is clean" --
rather than a proxy for it. An exclusion that stops working still leaves the
tree unchanged, so a check over the source tree would pass while the snapshot
leaked.

WHY THE PATTERN LIST IS GENERIC. A tracked file that lists the hostnames it
forbids publishes them on the next snapshot -- the exact failure it exists to
prevent. So the committed rules are STRUCTURAL (private address ranges, ssh
tunnel directives), which name nothing, and the specific strings come from
$AXL_SNAPSHOT_FORBIDDEN at release time -- a CI secret, never committed.
Structural rules alone already caught a leak §15.1's own inventory missed: two
real lab IPs, a service tag and an iDRAC credential pair in ROADMAP-Archive.md,
in a file no exclusion class covered.

DOCUMENTATION ADDRESSES ARE ALLOWLISTED BY VALUE, NOT BY FILE. `10.0.2.2` is
QEMU's user-net gateway and appears in half the networking docs; `192.168.1.1`
is the textbook example. Allowlisting the FILES those appear in would have
allowlisted ROADMAP-Archive.md too, and missed the leak.

Usage: scripts/check-snapshot-clean.py <snapshot-dir>
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# Private and CGNAT IPv4. CGNAT (100.64/10) is in here because a tailnet
# address is exactly the kind of thing that identifies a personal machine.
#
# 169.254/16 is deliberately NOT here. A link-local address is auto-assigned,
# non-routable off the link, and identifies no machine -- it is what a NIC
# gives itself when DHCP fails, which is why the netload docs and the TCP/UDP
# headers are full of them. Scanning for it produced only noise.
IPV4 = re.compile(
    r"\b(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}"
    r"|192\.168\.\d{1,3}\.\d{1,3}"
    r"|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}"
    r"|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d{1,3}\.\d{1,3})\b"
)

# WHERE THE IP SCAN APPLIES, and why it is not everywhere.
#
# A private address in a test fixture, an API docstring or vendored upstream
# code is an INPUT or an EXAMPLE -- mbedTLS's own x509 test data is full of
# them, and `axl_ipv4_parse_cidr()`'s docstring has to show an address to
# document itself. A private address in PROSE is the risk: session notes,
# runbooks and archived roadmaps are where a real machine gets written down,
# and that is exactly where the leak §15.1's inventory missed was found.
#
# So prose is scanned strictly, by value; everywhere else is scanned for the
# structural and secret patterns only. This is NOT the file-allowlist §15.1
# warned about -- that would have allowlisted the archived roadmap and missed
# the leak. Prose gets the strictest rule, not an exemption.
def scans_ips(rel: Path) -> bool:
    parts = rel.parts
    if parts and parts[0] == "deps":
        return False                      # vendored upstream; not ours to redact
    if parts and parts[0] == "docs":
        return True
    return len(parts) == 1 and rel.suffix.lower() in {".md", ".txt", ".rst"}

# Addresses that are documentation, not infrastructure. Each needs a reason:
# an allowlist without one grows until it allows everything.
DOC_ADDRESSES = {
    "10.0.2.2":      "QEMU user-net gateway (the host, from inside the guest)",
    "10.0.2.3":      "QEMU user-net DNS",
    "10.0.2.15":     "QEMU user-net guest address",
    "10.0.2.16":     "QEMU user-net, second guest",
    "10.0.0.1":      "textbook example address",
    "10.0.0.5":      "textbook example address",
    "192.168.0.1":   "textbook example address",
    "192.168.1.1":   "textbook example address",
    "192.168.1.42":  "textbook example address",
    "192.168.1.100": "textbook example address",
}

# Structural giveaways of a personal/tunnelled setup. These name nothing.
DIRECTIVES = re.compile(
    r"^\s*(ProxyJump|ProxyCommand|RemoteForward|LocalForward|IdentityFile)\s",
    re.M,
)

# Binary and asset types are read for strings but never for IPs -- a compiled
# fixture matching an address pattern by chance is noise, not a leak.
SKIP_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".pdf", ".efi",
                 ".o", ".a", ".so", ".gz", ".xz", ".zip", ".bin", ".fd"}


def fail(msg: str) -> None:
    print(f"check-snapshot-clean: {msg}", file=sys.stderr)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check-snapshot-clean.py <snapshot-dir>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])

    # "Found nothing" and "could not look" are the same empty output and
    # opposite facts. Establish the snapshot is real and non-trivial BEFORE
    # believing any silence from the scan below.
    if not root.is_dir():
        fail(f"{root} is not a directory -- nothing was scanned")
        return 1
    files = [p for p in root.rglob("*")
             if p.is_file() and p.suffix.lower() not in SKIP_SUFFIXES]
    if len(files) < 100:
        fail(f"{root} holds only {len(files)} scannable files -- that is not a"
             " source snapshot, and a clean result over it would mean nothing")
        return 1

    extra = [ln.strip() for ln in
             os.environ.get("AXL_SNAPSHOT_FORBIDDEN", "").splitlines()
             if ln.strip() and not ln.strip().startswith("#")]

    findings: list[str] = []
    for path in sorted(files):
        rel = path.relative_to(root)
        try:
            text = path.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue                      # binary or unreadable: not prose

        if scans_ips(rel):
            for m in IPV4.finditer(text):
                addr = m.group(0)
                if addr in DOC_ADDRESSES:
                    continue
                line = text.count("\n", 0, m.start()) + 1
                findings.append(f"{rel}:{line}: private address {addr}")

        if rel.parts and rel.parts[0] == "deps":
            continue                      # vendored: not ours to police

        for m in DIRECTIVES.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            findings.append(f"{rel}:{line}: ssh {m.group(1)} directive")

        # Never printed back: echoing the secret into a public build log would
        # publish it exactly as the snapshot would have.
        for i, needle in enumerate(extra):
            if needle in text:
                line = text.count("\n", 0, text.index(needle)) + 1
                findings.append(f"{rel}:{line}: forbidden string #{i + 1} "
                                "(from AXL_SNAPSHOT_FORBIDDEN)")

    if findings:
        fail(f"the snapshot carries {len(findings)} thing(s) that must not be "
             "published:")
        for f in findings:
            print(f"  {f}", file=sys.stderr)
        fail("Fix at the SOURCE (redact it) or exclude the file in "
             "release.yml's snapshot step. Do not add it to DOC_ADDRESSES "
             "unless it really is a documentation address.")
        return 1

    print(f"check-snapshot-clean: clean -- {len(files)} files scanned, "
          f"{len(DOC_ADDRESSES)} documentation addresses allowed, "
          f"{len(extra)} extra pattern(s) from the environment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
