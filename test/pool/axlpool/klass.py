"""klass.py -- what a pooled guest IS: its launch, and the key that names it.

A class is the machine a test needs: everything fixed when QEMU starts and
the firmware enumerates (docs/AXL-Test-Guest-Pool-Design.md §6.1). Its key
names a snapshot, so it must change whenever the machine would. It is taken
from a dry run of the EXACT launch a boot uses (launch_args, shared with
guest.py), with that run's temp dir, the class's own state and share
directories and the forwarded host port normalised away, plus what run-qemu.sh
reports beyond the argv: a digest of every file staged onto the boot disk
(launcher, Shell, its options, the start script, the agent) and the vars
template it copies -- and the bytes of every firmware file the argv loads
and the QEMU binary's identity. A rebuilt firmware, Shell, launcher or agent
therefore gets a new class and a fresh boot, never a stale snapshot.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

# The guest-side port the agent listens on; the host side is chosen per launch.
# Not 8080: 22 integration tests serve on guest 8080 (8443, 2222, 7000, 5640
# and 808x too), and a pooled one forwards to its own port beside the agent.
AGENT_GUEST_PORT = 4747
# The name the agent is staged under, whatever the caller's file is called:
# the start script runs it by this name.
AGENT_NAME = "agent.efi"
# Every pooled guest's Shell is the launcher's, started -nonest (the agent's
# outcomes are only definite there).
POOL_ENV = {"AXL_SHELL_LAUNCHER": "1", "AXL_SHELL_OPTIONS": "-nonest"}
# run-qemu.sh's guest RAM when no --mem is given (its MEM default).
DEFAULT_GUEST_RAM_MB = 512
# run-qemu.sh's own rule for --mem (it refuses anything else, a bare N included).
_MEM_RE = re.compile(r"^([1-9][0-9]*)([MG])$")


def mem_mb(value: str) -> int | None:
    """A run-qemu.sh --mem value (<N>M or <N>G) in MiB, or None when it is
    not one run-qemu.sh accepts."""
    m = _MEM_RE.match(value)
    if m is None:
        return None
    return int(m.group(1)) * (1024 if m.group(2) == "G" else 1)


def mem_values(args: tuple[str, ...]) -> list[str]:
    """Every --mem value in @args, in order ("" for a trailing --mem)."""
    return [args[i + 1] if i + 1 < len(args) else "" for i, a in enumerate(args) if a == "--mem"]


@dataclass(frozen=True)
class GuestSpec:
    """The machine a test needs. `extra_args` are run-qemu.sh options beyond
    the pool's own (launch_args), e.g. ("--cpu", "Haswell")."""

    arch: str                       # "X64" or "AARCH64"
    agent: Path                     # the agent image the guest runs
    extra_args: tuple[str, ...] = field(default_factory=tuple)
    # The agent's axl_net_set_latency mode: "always" (~1 ms round trips, a
    # 1 ms firmware tick) or "firmware" -- measured: at a 1 ms tick OVMF's
    # USB keyboard delivered 0 of 10 keys typed into a program the agent
    # ran, at the firmware's 10 ms 10 of 10.
    agent_latency: str = "always"
    # The checkout whose run-qemu.sh, firmware, launcher and Shell make the
    # machine (service.py TREES): a CI run of another commit gets its own.
    tree: Path = REPO

    @property
    def run_qemu(self) -> Path:
        return self.tree / "scripts" / "run-qemu.sh"

    @property
    def ram_mb(self) -> int:
        """The guest RAM the machine takes, in MiB: the LAST --mem in
        extra_args (run-qemu.sh's parse), else run-qemu.sh's default. What
        the pool's memory cap counts a guest as (pool.py mem_cap_mb). A
        class's --mem values are checked when the classes load."""
        vals = mem_values(self.extra_args)
        got = mem_mb(vals[-1]) if vals else None
        return got if got is not None else DEFAULT_GUEST_RAM_MB


def start_script(spec: GuestSpec) -> str:
    """The Shell script that starts the agent from whichever volume holds it
    (on AARCH64 the virtio-fs share enumerates first, as FS0)."""
    return "".join(f"if exist fs{i}:\\{AGENT_NAME} then\r\n"
                   f"  fs{i}:\\{AGENT_NAME} --port {AGENT_GUEST_PORT} "
                   f"--latency {spec.agent_latency}\r\nendif\r\n"
                   for i in range(8))


def launch_args(spec: GuestSpec, state: Path, share: Path) -> list[str]:
    """The run-qemu.sh arguments of @spec's launch, with @state holding its
    qcow2 images, vmstate, QMP socket, start script and staged agent, and
    @share its virtio-fs directory. Writes the start script and the agent
    copy into @state. The same list boots a guest (guest.py, plus
    --background) and is dry-run for its key: they cannot drift."""
    state.mkdir(parents=True, exist_ok=True)
    (state / "start.nsh").write_text(start_script(spec))
    shutil.copyfile(spec.agent, state / AGENT_NAME)
    return ["--arch", spec.arch, "--background", "--qcow2", "--state-dir", str(state),
            "--net", "--hostfwd-bind", "127.0.0.1", "--hostfwd", f"auto:{AGENT_GUEST_PORT}",
            "--qmp-socket", str(state / "qmp.sock"), "--mount", str(share),
            "--nsh", str(state / "start.nsh"),
            # A reset (system_reset, or a test's `reset`) REBOOTS the guest:
            # run-qemu.sh --background passes -no-reboot, and a later
            # -action wins.
            "--qemu-arg", "-action", "--qemu-arg", "reboot=reset",
            # A second monitor, for the pool's event listener (guest.py EVENTS_SOCK).
            "--qemu-arg", "-qmp", "--qemu-arg", f"unix:{state / 'events.sock'},server=on,wait=off",
            "--qemu-arg", "-blockdev", "--qemu-arg",
            f"driver=qcow2,node-name=vmstate,file.driver=file,file.filename={state / 'vmstate.qcow2'}",
            *spec.extra_args, str(state / AGENT_NAME)]


@dataclass(frozen=True)
class DryRun:
    """What run-qemu.sh would launch for a class, normalised."""

    argv: list[str]                 # QEMU argv: TMP, STATE, SHARE, PORT for the per-run parts
    staged: str                     # digest of the boot disk's files
    inputs: list[Path]              # files the run reads that the argv does not name


_PORT = re.compile(r"(hostfwd=tcp:[0-9.]*:)\d+-:")


def dry_run(spec: GuestSpec) -> DryRun:
    """Dry-run @spec's launch (launch_args) in throwaway state and share
    directories. Raises RuntimeError when run-qemu.sh refuses it."""
    with tempfile.TemporaryDirectory(prefix="axlpool-key.") as d:
        state, share = Path(d) / "state", Path(d) / "share"
        share.mkdir()
        out = subprocess.run([str(spec.run_qemu), *launch_args(spec, state, share)],
                             capture_output=True, text=True, cwd=spec.tree,
                             env=dict(os.environ, **POOL_ENV, QEMU_DRYRUN="1"), check=False)
    text = out.stdout + out.stderr
    argv = [ln[len("QEMU_DRYRUN: "):] for ln in text.splitlines() if ln.startswith("QEMU_DRYRUN: ")]
    facts = dict(ln.split("=", 1) for ln in text.splitlines() if ln.startswith("AXL_DRYRUN_"))
    if out.returncode != 0 or not argv or "AXL_DRYRUN_TMPDIR" not in facts \
            or "AXL_DRYRUN_STAGED" not in facts:
        raise RuntimeError(f"run-qemu.sh refused the launch (exit {out.returncode}):\n{text[-800:]}")
    tmp = facts["AXL_DRYRUN_TMPDIR"]

    def norm(a: str) -> str:
        a = a.replace(tmp, "TMP").replace(str(state), "STATE").replace(str(share), "SHARE")
        return _PORT.sub(r"\1PORT-:", a)

    inputs = [Path(facts["AXL_DRYRUN_INPUT"])] if facts.get("AXL_DRYRUN_INPUT") else []
    return DryRun([norm(a) for a in argv], facts["AXL_DRYRUN_STAGED"], inputs)


def dry_run_argv(spec: GuestSpec) -> list[str]:
    """dry_run(@spec).argv."""
    return dry_run(spec).argv


def file_digest(path: Path) -> str:
    """sha256 of a file's bytes."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _loaded_files(argv: list[str]) -> list[Path]:
    """Files the argv loads from outside the per-run paths: firmware, ROMs."""
    found: list[Path] = []
    for a in argv:
        for m in re.finditer(r"(?:file|romfile)=([^,]+)", a):
            p = Path(m.group(1))
            if m.group(1).startswith(("TMP", "STATE", "SHARE")) or not p.is_file():
                continue
            found.append(p)
    return found


def class_key(spec: GuestSpec) -> str:
    """The hex key naming @spec's class (module docstring)."""
    run = dry_run(spec)
    h = hashlib.sha256()
    h.update("\0".join(run.argv).encode())
    h.update(f"\0staged={run.staged}".encode())
    qemu = Path(run.argv[0])
    st = qemu.stat()
    h.update(f"\0qemu={qemu}:{st.st_size}:{st.st_mtime_ns}".encode())
    for f in [*_loaded_files(run.argv), *run.inputs]:
        h.update(f"\0{f.name}={file_digest(f)}".encode())
    return h.hexdigest()
