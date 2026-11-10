"""Booting a suite's guest for a run: the stale-guest cleanup, the build, the
boot and, for a kept guest, the state file a later ``--attach`` or
``explore`` reads. Shared by the suite runner and by ``explore --start``."""
from __future__ import annotations

import time
from pathlib import Path

from cvlib.cases import Suite
from cvlib.guest import Guest, GuestError


def boot_guest(suite: Suite, arch: str, state_file: Path, no_build: bool,
               keep: bool) -> tuple[Guest, dict[str, float]]:
    """Build and boot ``suite``'s guest; returns it and the timings. A guest
    left over from an earlier ``keep`` is stopped first -- never leaked."""
    timing: dict[str, float] = {}
    if state_file.is_file():
        try:
            Guest.attach(state_file, suite.name, arch).stop()
        except GuestError:
            pass
        state_file.unlink(missing_ok=True)
    if not no_build:
        t0 = time.monotonic()
        print(f"building ({arch}) ...", flush=True)
        suite.build(arch)
        timing["build"] = time.monotonic() - t0
    print(f"booting the {arch} guest ...", flush=True)
    gdir = state_file.parent
    spec = suite.boot(arch, gdir)
    guest = Guest.boot(suite.name, arch, spec, gdir, 7200 if keep else 1800)
    timing["boot"] = guest.state.boot_secs
    print(f"  booted in {guest.state.boot_secs:.1f}s", flush=True)
    if keep:
        guest.state.save(state_file)
    return guest, timing
