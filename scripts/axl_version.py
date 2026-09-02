# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AximCode
"""Resolve the AXL SDK version for a host-side Python tool.

The Python mirror of axl_version_string() in axl-common.sh, and it
resolves in the same order for the same reason: a checkout answers from
its own VERSION file, a staged or installed prefix from the
share/axl/version beside it, never the reverse. axl-cc once reported a
six-week-old staged version while running out of a tree that had just
built something else, and that string ends up in reports as evidence of
which build was measured.

Use it as argparse's version action so every command answers in the same
shape the target-side tools do:

    p.add_argument("--version", action="version",
                   version=f"axl-emulate {version_string()}")
"""

from __future__ import annotations

from pathlib import Path


def version_string() -> str:
    """Return the SDK version, or "unknown" rather than inventing one."""
    here = Path(__file__).resolve().parent

    # A checkout: scripts/ sits directly under the repo root.
    tree = here.parent / "VERSION"
    if tree.is_file():
        return tree.read_text(encoding="utf-8").strip()

    # A staged/installed prefix: share/axl/version sits above this file.
    for parent in here.parents:
        staged = parent / "share" / "axl" / "version"
        if staged.is_file():
            return staged.read_text(encoding="utf-8").strip()

    return "unknown"
