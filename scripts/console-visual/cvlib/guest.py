"""The guest: ONE QEMU per arch per run, booted ONLY through
scripts/run-qemu.sh (``--background --gpu --net --hostfwd auto:N --extra
--nsh --serial-log --monitor-socket``), and driven over that HMP socket for
its whole life.

Boot is paid once per run. ``keep`` leaves the guest running and records it
in a state file; ``attach`` reuses it, so iterating on one case costs no
boot. The guest's own lifetime is still bounded by run-qemu.sh's
``--timeout`` watchdog, so an abandoned kept guest goes away on its own.
"""
from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[3]
RUN_QEMU = PROJECT_DIR / "scripts" / "run-qemu.sh"


class GuestError(RuntimeError):
    """The guest could not be booted, attached or driven."""


@dataclass
class BootSpec:
    """What a case table needs booted: an .efi run from a custom startup.nsh
    with files staged next to it, host ports forwarded, and the serial line
    that says it is ready."""

    efi: Path
    nsh_lines: list[str]
    extras: list[Path]
    hostfwd: list[int]
    ready: re.Pattern[bytes]
    ready_secs: float = 120.0


@dataclass
class GuestState:
    """What a later ``attach`` needs to reach a ``keep`` guest."""

    suite: str
    arch: str
    qemu_pid: int
    monitor: str
    serial_log: str
    workdir: str
    boot_secs: float
    ports: dict[str, int] = field(default_factory=lambda: {})

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(asdict(self), indent=2))

    @staticmethod
    def load(path: Path) -> GuestState:
        raw = json.loads(path.read_text())
        return GuestState(**raw)


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def build_prefix(arch: str) -> Path:
    out = subprocess.run([str(PROJECT_DIR / "scripts" / "build-prefix.sh"), arch, "--abs"],
                         check=True, capture_output=True, text=True)
    return Path(out.stdout.strip())


class Guest:
    """A booted (or attached) guest."""

    def __init__(self, state: GuestState) -> None:
        self.state = state
        self.serial = Path(state.serial_log)

    @staticmethod
    def boot(suite: str, arch: str, spec: BootSpec, workdir: Path,
             timeout_s: int) -> Guest:
        if not spec.efi.is_file():
            raise GuestError(f"{spec.efi} is missing")
        workdir.mkdir(parents=True, exist_ok=True)
        nsh = workdir / "startup.nsh"
        nsh.write_text("\r\n".join(spec.nsh_lines) + "\r\n")
        monitor = workdir / "monitor.sock"
        serial = workdir / "serial.log"
        qarch = {"x64": "X64", "aa64": "AARCH64"}[arch]
        cmd: list[str] = [str(RUN_QEMU), "--arch", qarch, "--gpu", "--background",
                          "--timeout", str(timeout_s), "--serial-log", str(serial),
                          "--nsh", str(nsh), "--monitor-socket", str(monitor)]
        if spec.hostfwd:
            cmd.append("--net")
        for port in spec.hostfwd:
            cmd += ["--hostfwd", f"auto:{port}"]
        for e in spec.extras:
            cmd += ["--extra", str(e)]
        cmd.append(str(spec.efi))
        t0 = time.monotonic()
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise GuestError(f"run-qemu.sh failed:\n{res.stdout}\n{res.stderr}")
        kv = dict(line.split("=", 1) for line in res.stdout.splitlines() if "=" in line)
        want = ["QEMU_PID", "MONITOR_SOCKET"] + [f"HOSTFWD_{p}" for p in spec.hostfwd]
        missing = [k for k in want if k not in kv]
        if missing:
            raise GuestError(f"run-qemu.sh printed no {', '.join(missing)}:\n{res.stdout}")
        state = GuestState(suite=suite, arch=arch, qemu_pid=int(kv["QEMU_PID"]),
                           monitor=kv["MONITOR_SOCKET"], serial_log=str(serial),
                           workdir=str(workdir), boot_secs=0.0,
                           ports={str(p): int(kv[f"HOSTFWD_{p}"]) for p in spec.hostfwd})
        guest = Guest(state)
        try:
            guest.wait_serial(spec.ready, spec.ready_secs, "the guest's ready line")
        except GuestError:
            guest.stop()
            raise
        state.boot_secs = time.monotonic() - t0
        return guest

    @staticmethod
    def attach(state_file: Path, suite: str, arch: str) -> Guest:
        if not state_file.is_file():
            raise GuestError(f"attach: no kept guest ({state_file} missing; run with --keep)")
        state = GuestState.load(state_file)
        if (state.suite, state.arch) != (suite, arch):
            raise GuestError(f"attach: the kept guest is {state.suite}/{state.arch}")
        if not pid_alive(state.qemu_pid):
            raise GuestError(f"attach: the kept guest (pid {state.qemu_pid}) is gone")
        return Guest(state)

    def stop(self) -> None:
        """Kill QEMU (run-qemu's `timeout` wrapper forwards the signal); wait."""
        pid = self.state.qemu_pid
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            return
        deadline = time.monotonic() + 10
        while pid_alive(pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        if pid_alive(pid):
            os.kill(pid, signal.SIGKILL)

    def alive(self) -> bool:
        return pid_alive(self.state.qemu_pid)

    def serial_bytes(self, offset: int = 0) -> bytes:
        with self.serial.open("rb") as f:
            f.seek(offset)
            return f.read()

    def wait_serial(self, pattern: re.Pattern[bytes], secs: float, what: str) -> None:
        deadline = time.monotonic() + secs
        while time.monotonic() < deadline:
            if self.serial.exists() and pattern.search(self.serial_bytes()):
                return
            if not self.alive():
                raise GuestError(f"QEMU exited while waiting for {what}")
            time.sleep(0.05)
        raise GuestError(f"timed out ({secs:.0f}s) waiting for {what}")

    def monitor(self, command: str, timeout_s: float = 10.0) -> str:
        """One HMP command over run-qemu's --monitor-socket; its output.

        A connection per command: HMP greets with a banner and a ``(qemu)``
        prompt, then prints the command's output and the next prompt.
        """
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(timeout_s)
            s.connect(self.state.monitor)
            buf = b""
            while b"(qemu) " not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    raise GuestError("monitor closed before its prompt")
                buf += chunk
            s.sendall(command.encode() + b"\n")
            buf = b""
            while b"(qemu) " not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        return buf.decode(errors="replace")
