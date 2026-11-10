#!/usr/bin/env python3
"""klass-cases.py -- axlpool.klass's class key, on the host (no guest).

Usage: klass-cases.py AGENT_X64_EFI SHELL_LAUNCHER_EFI

Prints `  PASS:`/`  FAIL:` per case and `klass-cases: <p> passed, <f> failed`.
Every case compares whole values.
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration" / "lib"))
from axlpool.klass import GuestSpec, class_key, dry_run_argv  # noqa: E402
from checks import Cases  # noqa: E402


def main() -> int:
    agent = Path(sys.argv[1])
    # The built Shell launcher, from the caller: it lives in whichever tree
    # the build made (scripts/build-prefix.sh), not at a fixed path.
    # A hard-coded out/native-x64 read a stale make-built copy on a dev box
    # and failed on CI's fresh CMake checkout.
    repo_launcher = Path(sys.argv[2])
    c = Cases()
    spec = GuestSpec("X64", agent)
    argv = dry_run_argv(spec)
    no_paths: list[str] = []
    c.check("the dry-run argv holds no per-run path (run temp dir, class state, share)",
            [a for a in argv if "axl-qemu." in a or "axlpool-key." in a or "/tmp/" in a], no_paths)
    c.check("the vars pflash is qcow2, in the class's normalised state dir",
            "if=pflash,format=qcow2,file=STATE/vars.qcow2" in argv, True)
    c.check("the forwarded host port is normalised, and bound to loopback",
            [a for a in argv if "hostfwd=" in a], ["user,id=net0,hostfwd=tcp:127.0.0.1:PORT-:4747"])

    k = class_key(spec)
    c.check("the key is a sha256 hex digest", (len(k), all(ch in "0123456789abcdef" for ch in k)),
            (64, True))
    c.check("the key is stable across launches (new temp dirs and ports each time)",
            class_key(spec), k)
    c.check("another agent latency (the start script differs) is another class",
            class_key(GuestSpec("X64", agent, agent_latency="firmware")) != k, True)
    c.check("another CPU is another class",
            class_key(GuestSpec("X64", agent, ("--cpu", "Haswell"))) != k, True)
    with tempfile.TemporaryDirectory() as d:
        same = Path(d) / "agent.efi"
        shutil.copyfile(agent, same)
        c.check("the same agent bytes at another path: the same class",
                class_key(GuestSpec("X64", same)), k)
        with same.open("ab") as f:
            f.write(b"\0")
        c.check("a rebuilt agent (other bytes) is another class",
                class_key(GuestSpec("X64", same)) != k, True)
        # A rebuilt firmware: OVMF_CODE points run-qemu.sh at a copy whose
        # bytes differ (the dry run never boots it).
        code = next(a.split("file=", 1)[1] for a in argv
                    if a.startswith("if=pflash") and "readonly=on" in a)
        # A rebuilt Shell launcher (a file staged on the boot disk, named by
        # no argv): the same path, other bytes.
        launcher = Path(d) / "launcher.efi"
        shutil.copyfile(repo_launcher, launcher)
        os.environ["AXL_SHELL_LAUNCHER_BIN"] = str(launcher)
        try:
            before = class_key(spec)
            with launcher.open("ab") as f:
                f.write(b"\0")
            c.check("a rebuilt launcher (a staged file, same path) is another class",
                    class_key(spec) != before, True)
        finally:
            del os.environ["AXL_SHELL_LAUNCHER_BIN"]
        # run-qemu.sh takes OVMF_VARS beside OVMF_CODE: <name>_CODE.fd ->
        # <name>_VARS.fd. The vars image is the X64 default's.
        fw = Path(d) / "fw_CODE.fd"
        shutil.copyfile(code, fw)
        shutil.copyfile(Path(code).parent / "edk2-i386-vars.fd", Path(d) / "fw_VARS.fd")
        os.environ["OVMF_CODE"] = str(fw)
        try:
            # The same PATH before and after: only the bytes change, so the
            # key must be reading them (the path alone is in the argv).
            before = class_key(spec)
            with fw.open("r+b") as f:
                f.seek(4096)
                f.write(b"\xa5")
            c.check("a rebuilt firmware (other code bytes, same path) is another class",
                    class_key(spec) != before, True)
            before = class_key(spec)
            with (Path(d) / "fw_VARS.fd").open("r+b") as f:
                f.seek(100)
                f.write(b"\x5a")
            c.check("another vars template (same path, other bytes) is another class",
                    class_key(spec) != before, True)
        finally:
            del os.environ["OVMF_CODE"]
    # Per tree: a spec names its tree, and every launch and key uses THAT
    # tree's run-qemu.sh -- here a stand-in tree whose run-qemu.sh is a stub.
    with tempfile.TemporaryDirectory() as d:
        stub = Path(d) / "scripts" / "run-qemu.sh"
        stub.parent.mkdir()
        stub.write_text("#!/bin/bash\n"
                        "echo 'QEMU_DRYRUN: /bin/true'\necho 'QEMU_DRYRUN: -name'\n"
                        "echo 'QEMU_DRYRUN: other-tree'\n"
                        "echo AXL_DRYRUN_TMPDIR=/nonexistent\necho AXL_DRYRUN_STAGED=" + "0" * 64 + "\n")
        stub.chmod(0o755)
        other = GuestSpec("X64", agent, tree=Path(d))
        c.check("a spec's tree: its dry run is that tree's run-qemu.sh",
                dry_run_argv(other), ["/bin/true", "-name", "other-tree"])
        c.check("... and so is its key", class_key(other) != k, True)
    try:
        dry_run_argv(GuestSpec("X64", agent, ("--no-such-option",)))
        c.check("options run-qemu.sh refuses raise", "no error", "RuntimeError")
    except RuntimeError:
        c.check("options run-qemu.sh refuses raise", "RuntimeError", "RuntimeError")
    print(f"klass-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
