#!/usr/bin/env python3
"""check-log-linkage.py — an image that logs must carry the engine that logs.

`axl_error` / `axl_warning` / `axl_info` / `axl_debug` / `axl_trace` expand to
`axl_log_full`, which since 2026-08-19 is a trampoline in `axl-log-emit.o` over
a WEAK `_axl_log_vdispatch` defined in `axl-log.o` (see
`src/log/axl-log-dispatch.h`). That is what lets `--minimal-runtime` drop ~6 KB.
It also means the log layer is now one `-u` away from being absent, and an
image that loses it **fails silently**: every record is discarded, nothing
warns, and no test that is not specifically watching for it can tell.

So the linkage is checked on the ARTIFACT, not on the command line that
produced it. Reading the built `.so` covers every producer at once -- the
build's link rules (cmake/AxlImages.cmake), `axl-cc`, and the CMake package
`install.sh` generates -- which a grep of any one of them would not:
`check-flag-parity`'s whole reason for existing is that those three copies
drift.

The rule, per image:

    axl_log_full DEFINED  =>  _axl_log_vdispatch DEFINED

The left side is itself the "does this image log" test, because
`axl-log-emit.o` is pulled only by a reference to an emitter. An image that
never logs (`sdk/examples/hello-minimal.c` links no libaxl at all) has neither
symbol and is not the subject of the rule.

`SILENT_BY_DESIGN` names the images that are deliberately engine-less. It is a
list of exactly one -- the fixture whose entire purpose is to BE the silent
case for `test-minimal-log-qemu.sh` -- and an entry here is a claim that some
test asserts the silence, not a way to make this script stop complaining.

Reads the `.so` rather than the `.efi`: every `.efi` is `--strip-all`ed
(`check-pe-stripped` enforces it), so the PE has no symbol table left to read.
The `.so` is the same link, one step earlier.

**A local pass reads the images the tree last linked.** Under the retired
Makefile that was a real hole: images did not depend on the Makefile, so
editing a link macro relinked NOTHING, and sabotaging `LINK_EFI_DRIVER` went
undetected until `driver.efi` was deleted. Ninja re-runs a rule whose command
line changed, so an edited link rule in cmake/AxlImages.cmake relinks; a
change it cannot see (a `cmake -P` script's body) still needs the image
`rm`ed, or the build tree deleted. CI builds from scratch and so always reads
the current rules.

Usage: check-log-linkage.py IMAGE.efi [IMAGE.efi ...]
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

EMITTER = "axl_log_full"
ENGINE = "_axl_log_vdispatch"

# basename (without extension) -> why it is allowed to have no engine
SILENT_BY_DESIGN: dict[str, str] = {
    "minimal-log-off": (
        "the engine-less half of test-minimal-log-qemu.sh, which asserts it "
        "logs nothing AND still runs to completion"
    ),
}

# `nm` prints a definition as `<addr> <type> <name>`; T/t is text, W/V weak
# with a value. An undefined reference has no address and type U (or w for an
# unresolved weak one) -- which is exactly the state under test, so the
# distinction is the whole check and cannot be loosened to a bare name grep.
_DEFINED = re.compile(r"^[0-9a-fA-F]+\s+[TtWVRrDd]\s+(\S+)$")


def defined_symbols(so_path: Path, nm: str) -> set[str]:
    """Names this image DEFINES. Empty set if nm could not read it."""
    try:
        out = subprocess.run([nm, str(so_path)], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {m.group(1) for line in out.splitlines()
            if (m := _DEFINED.match(line.strip()))}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2

    # The same cross toolchain that produced the images. CROSS is exported by
    # the gate's CTest entry (cmake/AxlIntegrationTests.cmake.in); a bare `nm`
    # on a cross object reads fine for symbol names, so the fallback is safe
    # rather than merely convenient.
    nm = os.environ.get("CROSS", "") + "nm"

    failures: list[str] = []
    checked = 0
    silent = 0

    for arg in argv[1:]:
        efi = Path(arg)
        so = efi.with_suffix(".so")
        if not so.exists():
            failures.append(f"{efi}: no .so beside it — nothing to read "
                            f"(the .efi is stripped by design)")
            continue

        defined = defined_symbols(so, nm)
        if not defined:
            failures.append(f"{so}: '{nm}' produced no symbols — the check "
                            f"cannot see this image, which is worse than a "
                            f"failure it could report")
            continue

        if EMITTER not in defined:
            # Links no emitter, so it cannot log; not the subject of the rule.
            continue

        checked += 1
        name = efi.stem
        has_engine = ENGINE in defined

        if name in SILENT_BY_DESIGN:
            silent += 1
            if has_engine:
                failures.append(
                    f"{efi}: listed in SILENT_BY_DESIGN but DOES link "
                    f"{ENGINE}.\n"
                    f"    That entry claims a test asserts this image is "
                    f"silent; the image is not.\n"
                    f"    Either the link gained a pull it should not have, "
                    f"or the entry is stale.")
            continue

        if not has_engine:
            failures.append(
                f"{efi}: references {EMITTER} but does not link {ENGINE}.\n"
                f"    Every log call in this image is discarded, silently.\n"
                f"    In-tree: the link rule is missing AXL_LOG_ENGINE_PULL "
                f"(cmake/AxlImages.cmake).\n"
                f"    Via axl-cc: pass --minimal-runtime=log, or "
                f"--minimal-runtime=nolog\n"
                f"    if the silence is intended (and then say so in "
                f"SILENT_BY_DESIGN here).")

    if failures:
        print("check-log-linkage: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    # A control in the output itself: if `silent` ever reads 0 the negative
    # case stopped being built, and every remaining PASS is only telling us
    # that images which link the engine link the engine.
    print(f"check-log-linkage: clean — {checked} image(s) log, "
          f"{checked - silent} carry the engine, {silent} silent by design")
    if silent == 0:
        print("check-log-linkage: WARNING — no silent-by-design image was "
              "checked, so the engine-less case went unexercised",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
