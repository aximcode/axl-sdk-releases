#!/usr/bin/env python3
"""classes-cases.py -- axlpool.classes: the classes file, on the host.

Usage: classes-cases.py

Loads test/pool/classes.toml (it must load, and its classes resolve on both
arches) and a set of small TOML texts, each pinning one rule of the format
-- resolution, inheritance, per-arch arguments, and every refusal, whose
message is compared WHOLE: a typo in a classes file must never silently
become a different machine.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.classes import ClassError, ClassSet, load, loads  # noqa: E402
from checks import Cases  # noqa: E402

AGENT = Path("/agents/agent.efi")


def refused(text: str) -> str:
    """The ClassError message @text's load raises, or "loaded"."""
    try:
        loads(text, "t.toml")
        return "loaded"
    except ClassError as e:
        return str(e)
    except Exception as e:  # any other failure is a wrong answer, reported as one
        return f"{type(e).__name__}: {e}"


def main() -> int:
    c = Cases()
    repo = load(Path(__file__).resolve().parent / "classes.toml")
    # THE POOLED NIC IS THE BOOTED ONE: a pooled test runs on the NIC its
    # booted twin gets (common-test.sh _test_nic_device: e1000 -- QEMU's iPXE
    # ROM -- on X64, virtio-net on AARCH64), or the e1000/iPXE paths (the SNP
    # 15-frame limit test-http-throughput guards) go untested wherever the
    # pool serves. run-qemu.sh's own default is virtio-net.
    for arch, args in (("X64", ("--bridges", "--nic-model", "e1000")), ("AARCH64", ("--bridges",))):
        spec = repo.spec("standard", arch, AGENT)
        c.check(f"the repo's classes.toml: `standard` resolves on {arch}, on the booted guest's NIC",
                (spec.arch, spec.agent, spec.extra_args, spec.agent_latency),
                (arch, AGENT, args, "always"))
    c.check("...and `unit` inherits it (X64 e1000)",
            "e1000" in repo.spec("unit", "X64", AGENT).extra_args, True)

    here = Path(__file__).resolve().parents[2]
    c.check("load(): the classes file's tree is where it sits (<tree>/test/pool/classes.toml)",
            (repo.tree, repo.spec("standard", "X64", AGENT).tree), (here, here))
    c.check("loads(): the tree is given",
            loads("[class.a]\n", "t.toml", tree=Path("/trees/b")).spec("a", "X64", AGENT).tree,
            Path("/trees/b"))
    s: ClassSet = loads('''
[defaults]
slots = 2

[class.base]
args = ["--bridges"]

[class.base.arch.AARCH64]
args = ["--cpu", "max"]

[class.typing]
extends = "base"
agent_latency = "firmware"

[class.haswell]
extends = "base"
arches = ["X64"]
args = ["--cpu", "Haswell"]
keep = "lease"
slots = 1
''', "t.toml")
    c.check("a class's args, then its arch's",
            (s.spec("base", "X64", AGENT).extra_args, s.spec("base", "AARCH64", AGENT).extra_args),
            (("--bridges",), ("--bridges", "--cpu", "max")))
    c.check("extends appends to the parent's args",
            s.spec("haswell", "X64", AGENT).extra_args, ("--bridges", "--cpu", "Haswell"))
    h = s.get("haswell")
    b = s.get("base")
    c.check("defaults fill what a class leaves out; a class's own values win",
            ((b.slots, b.keep, b.reset), (h.slots, h.keep, h.reset)),
            ((2, "idle", "snapshot"), (1, "lease", "snapshot")))
    c.check("names, sorted", s.names(), ["base", "haswell", "typing"])
    c.check("machine_args: the run-qemu.sh options of a class on an arch (what spec() boots)",
            (s.machine_args("haswell", "X64"), s.machine_args("base", "AARCH64")),
            (("--bridges", "--cpu", "Haswell"), ("--bridges", "--cpu", "max")))
    c.check("agent_latency: defaults to always, inherited, set per class",
            (s.spec("base", "X64", AGENT).agent_latency, s.spec("typing", "X64", AGENT).agent_latency),
            ("always", "firmware"))

    # The guest RAM a class's machine takes, which the pool's memory cap
    # counts (pool.py mem_cap_mb): run-qemu.sh's default 512M unless its
    # args say otherwise -- the LAST --mem wins (an arch's args come after
    # the class's), in run-qemu.sh's units: NM or NG (it refuses a bare N).
    m = loads('''
[class.plain]

[class.big]
args = ["--mem", "768M"]

[class.huge]
extends = "big"
args = ["--mem", "2G"]

[class.gig]
args = ["--mem", "1G"]

[class.mixed]
args = ["--mem", "768M"]

[class.mixed.arch.AARCH64]
args = ["--mem", "1G"]
''', "m.toml")
    c.check("ram_mb: run-qemu.sh's 512 by default; --mem in M or G; the last wins",
            [m.spec(n, "X64", AGENT).ram_mb for n in ("plain", "big", "huge", "gig", "mixed")]
            + [m.spec("mixed", "AARCH64", AGENT).ram_mb],
            [512, 768, 2048, 1024, 768, 1024])

    for name, text, want in (
        ("an unknown class", None, "t.toml: no class `nope` (known: base, haswell, typing)"),
        ("an arch the class excludes", None,
         "t.toml: class `haswell` is not for AARCH64 (arches: X64)"),
        ("an unknown key", '[class.a]\nargz = ["--bridges"]\n',
         "t.toml: class `a`: unknown key `argz` (known: agent_latency, args, arch, arches, extends, keep, reset, slots)"),
        ("a bad agent_latency", '[class.a]\nagent_latency = "fast"\n',
         "t.toml: class `a`: agent_latency must be one of always, window, firmware -- got `fast` "
         "(firmware for a class whose tests type on a USB keyboard: a 1 ms tick loses keys)"),
        ("an unknown parent", '[class.a]\nextends = "b"\n',
         "t.toml: class `a` extends `b`, which is not defined"),
        ("an extends cycle", '[class.a]\nextends = "b"\n[class.b]\nextends = "a"\n',
         "t.toml: classes extend in a cycle: a -> b -> a"),
        ("a bad keep", '[class.a]\nkeep = "forever"\n',
         "t.toml: class `a`: keep must be one of idle, lease -- got `forever`"),
        ("a bad reset", '[class.a]\nreset = "fresh"\n',
         "t.toml: class `a`: reset must be one of snapshot, reuse, reset -- got `fresh` "
         "(a test that needs a fresh boot says guest=none, and is not pooled)"),
        ("slots below 1", '[class.a]\nslots = 0\n',
         "t.toml: class `a`: slots must be an integer of at least 1 -- got `0`"),
        ("args not a list of strings", '[class.a]\nargs = "--bridges"\n',
         "t.toml: class `a`: args must be a list of strings"),
        ("an unknown arch", '[class.a]\narches = ["RISCV64"]\n',
         "t.toml: class `a`: unknown arch `RISCV64` (known: AARCH64, X64)"),
        ("an unknown top-level table", '[clas.a]\n',
         "t.toml: unknown table `clas` (known: class, defaults)"),
        ("a class named none (reserved: a test's guest=none)", '[class.none]\n',
         "t.toml: `none` is reserved (a test's guest=none means not pooled)"),
        ("a --mem the pool cannot count", '[class.a]\nargs = ["--mem", "lots"]\n',
         "t.toml: class `a`: --mem `lots` is not a size run-qemu.sh takes (<N>M or <N>G)"),
        ("a bare --mem (run-qemu.sh refuses it)", '[class.a]\nargs = ["--mem", "1024"]\n',
         "t.toml: class `a`: --mem `1024` is not a size run-qemu.sh takes (<N>M or <N>G)"),
        ("a --mem with no value", '[class.a]\nargs = ["--bridges", "--mem"]\n',
         "t.toml: class `a`: --mem `` is not a size run-qemu.sh takes (<N>M or <N>G)"),
        # The CTest registration names a class's resource type with the name
        # verbatim (guest_<class>_<arch>): CTest takes [a-z_][a-z0-9_]* only.
        ("a class name CTest cannot carry", '[class.plain-firmware]\n',
         "t.toml: class `plain-firmware`: a class name is lower-case letters, digits and "
         "underscores, starting with a letter (it names a CTest resource type)"),
        ("an upper-case class name", '[class.Gpu]\n',
         "t.toml: class `Gpu`: a class name is lower-case letters, digits and "
         "underscores, starting with a letter (it names a CTest resource type)"),
        # run-qemu.sh reads a lease's class options one per line
        # (AXL_POOL_CLASS_ARGS): a newline inside one would split it.
        ("a newline inside an arg", '[class.a]\nargs = ["--qemu-arg", "a\\nb"]\n',
         "t.toml: class `a`: args: an arg may not contain a newline -- got 'a\\nb'"),
        ("a newline inside an arch's arg", '[class.a.arch.X64]\nargs = ["x\\ny"]\n',
         "t.toml: class `a`: arch.X64.args: an arg may not contain a newline -- got 'x\\ny'"),
        ("not TOML at all", '[class.a\n',
         "t.toml: not valid TOML: Expected ']' at the end of a table declaration (at line 1, column 9)"),
    ):
        if text is None:
            try:
                s.spec("nope" if "unknown class" in name else "haswell",
                       "X64" if "unknown class" in name else "AARCH64", AGENT)
                got = "resolved"
            except ClassError as e:
                got = str(e)
        else:
            got = refused(text)
        c.check(f"refused: {name}", got, want)
    print(f"classes-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
