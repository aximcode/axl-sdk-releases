"""The case-table runner: one guest, every scenario of a suite, content-gated
waits, captures, diffs, timing, and a heartbeat for long runs."""
from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from cvlib.cases import Act, ClientContext, Cond, Scenario, Step, Suite
from cvlib.clients import Client, Tmux
from cvlib.diff import DiffResult, compare, report
from cvlib.grid import Grid, wait_for
from cvlib.guest import Guest, GuestError
from cvlib.oracles import GridOracle, PictureOracle
from cvlib.render import write_pictures

POLL = 0.05
# Verdicts that need no picture and do not count as a difference.
OK_VERDICTS = ("MATCH", "CLOSED", "BY-DESIGN")
SETTLE_SECS = 0.3  # both grids unchanged this long = the step has finished painting
SETTLE_MAX = 4.0


@dataclass
class StepResult:
    scenario: str
    step: str
    session: str
    verdict: str
    char_diffs: int
    colour_diffs: int
    unknown: int
    known: list[str]
    client_secs: float | None
    oracle_secs: float | None
    settled: bool
    files: list[str]
    note: str = ""
    target: bool = True  # the session the step's actions went to (its time is measured)
    # An unclassified difference or a bug: the run fails. A step that differs
    # only by classified quirks does not (DiffResult.fails).
    fails: bool = False


@dataclass
class ScenarioResult:
    name: str
    what: str
    secs: float = 0.0
    qemu_cores: float = 0.0  # QEMU's host CPU over the scenario, in cores
    steps: list[StepResult] = field(default_factory=lambda: [])
    error: str = ""


def qemu_cpu_secs(pid: int) -> float:
    """utime+stime of run-qemu's wrapper and everything under it (QEMU)."""
    total = 0.0
    tick = os.sysconf("SC_CLK_TCK")
    for p in _descendants(pid):
        try:
            f = Path(f"/proc/{p}/stat").read_text().rsplit(")", 1)[1].split()
            total += (int(f[11]) + int(f[12])) / tick
        except (OSError, IndexError, ValueError):
            pass
    return total


def _descendants(pid: int) -> list[int]:
    out = [pid]
    i = 0
    while i < len(out):
        try:
            for t in Path(f"/proc/{out[i]}/task").iterdir():
                out += [int(k) for k in (t / "children").read_text().split()]
        except OSError:
            pass
        i += 1
    return out


class Heartbeat:
    """A line to a progress log every interval: where the run is, whether
    QEMU lives, and its host CPU since the last beat (from /proc, not ps)."""

    def __init__(self, log: Path | None, interval: float, guest_pid: int) -> None:
        self.log, self.interval, self.pid = log, interval, guest_pid
        self.where = "start"
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        if self.log:
            self.thread.start()

    def _run(self) -> None:
        last_cpu, last_t = qemu_cpu_secs(self.pid), time.monotonic()
        while not self.stop.wait(self.interval):
            cpu, t = qemu_cpu_secs(self.pid), time.monotonic()
            cores = (cpu - last_cpu) / max(t - last_t, 1e-3)
            last_cpu, last_t = cpu, t
            alive = os.path.exists(f"/proc/{self.pid}")
            assert self.log is not None
            with self.log.open("a") as f:
                f.write(f"{time.strftime('%H:%M:%S')} heartbeat: at {self.where}; "
                        f"qemu pid {self.pid} {'alive' if alive else 'GONE'}; "
                        f"qemu cpu {cores:.2f} cores over {self.interval:.0f}s\n")

    def end(self) -> None:
        self.stop.set()


class Runner:
    def __init__(self, suite: Suite, guest: Guest, outdir: Path, shots: bool,
                 scale: float) -> None:
        self.suite, self.guest, self.outdir = suite, guest, outdir
        self.shots, self.scale = shots, scale
        self.cols, self.rows = suite.console_size(guest)
        self.oracle: GridOracle = suite.grid_oracle(guest, self.cols, self.rows)
        self.picture: PictureOracle | None = (
            suite.picture_oracle(guest, self.cols, self.rows) if suite.picture_oracle else None)
        self.tmux = Tmux(f"console-visual-{os.getpid()}", outdir / "tmux.conf")
        self.sessions: dict[str, Client] = {}
        self.heartbeat: Heartbeat | None = None
        self.step_notes: list[str] = []

    # --- sessions ----------------------------------------------------------

    def open_session(self, sc: Scenario, name: str, sdir: Path) -> Client:
        g = self.oracle.grid()  # the console as it is NOW (a `mode` reshapes it)
        size = sc.sessions.get(name) or (g.cols, g.rows)
        ctx = ClientContext(self.tmux, self.guest, f"{sc.name}-{name}", size,
                            sdir / f"{name}.raw")
        client = self.suite.client(ctx)
        client.open()
        took = wait_for(lambda: self._prompt(client.grid()) or self._held(client.grid())
                        or client.dead(), 20 * self.scale)
        if took is None or client.dead():
            raise GuestError(f"session {name}: never showed a prompt\n{client.grid().text()}")
        self.sessions[name] = client
        return client

    def close_sessions(self) -> None:
        for c in self.sessions.values():
            c.close()
        self.sessions.clear()

    # --- conditions --------------------------------------------------------

    def _prompt(self, g: Grid) -> bool:
        return bool(self.suite.prompt.match(g.cursor_line_prefix()))

    def _held(self, g: Grid) -> bool:
        return self.suite.held is not None and bool(self.suite.held.search(g.text()))

    def _holds(self, c: Cond, g: Grid) -> bool:
        lines = g.lines()
        if c.kind == "line":
            return c.arg in lines
        if c.kind == "has":
            return any(c.arg in ln for ln in lines)
        if c.kind == "gone":
            return not any(c.arg in ln for ln in lines)
        if c.kind == "joined":
            return c.arg in "".join(ln.ljust(g.cols) for ln in lines)
        if c.kind == "prompt":
            return self._prompt(g)
        if c.kind == "cursorline":
            return c.arg in g.cursor_line_prefix()
        raise ValueError(f"unknown condition {c.kind}")

    def _client_ok(self, st: Step, client: Client) -> bool:
        g = client.grid()
        held = self._held(g)
        for c in st.until:
            if c.kind == "dead":
                if not self.sessions[c.arg].dead():
                    return False
            elif held and not c.client_only:
                continue  # held: it shows the notice, not the console
            elif not self._holds(c, g):
                return False
        return True

    def _oracle_ok(self, st: Step) -> bool:
        g = self.oracle.grid()
        return all(self._holds(c, g) for c in st.until if not c.client_only)

    # --- actions -----------------------------------------------------------

    def _act(self, a: Act, sc: Scenario, sdir: Path) -> None:
        if a.kind == "open":
            self.open_session(sc, str(a.arg), sdir)
            return
        client = self.sessions[a.on]
        if a.kind == "cmd":
            # {boot}: the console size the guest booted at, as "COLS ROWS"
            # (a case that reshapes the console puts it back with it).
            client.act("type", str(a.arg).replace("{boot}", f"{self.cols} {self.rows}"))
            client.act("keys", ("Enter",))
        elif a.kind == "keylat":
            self._key_latency(client, str(a.arg))
        else:
            client.act(a.kind, a.arg)

    def _key_latency(self, client: Client, text: str) -> None:
        """Type one key at a time; time each echo on the client and on the
        oracle (the "lag" a typist feels, against the firmware's own)."""
        c_ms: list[float] = []
        o_ms: list[float] = []
        base_c = client.grid().cursor_line_prefix()
        base_o = self.oracle.grid().cursor_line_prefix()
        for i, ch in enumerate(text):
            want_c, want_o = base_c + text[: i + 1], base_o + text[: i + 1]
            got: dict[str, float] = {}
            t0 = time.monotonic()
            client.act("type", ch)

            def echoed() -> bool:
                now = time.monotonic() - t0
                if "c" not in got and client.grid().cursor_line_prefix() == want_c:
                    got["c"] = now
                if "o" not in got and self.oracle.grid().cursor_line_prefix() == want_o:
                    got["o"] = now
                return len(got) == 2

            wait_for(echoed, 5 * self.scale, 0.005)
            c_ms.append(got.get("c", float("inf")) * 1000)
            o_ms.append(got.get("o", float("inf")) * 1000)
        probe = _probe_cost_ms(client)
        self.step_notes.append(
            f"per-key echo over {len(text)} keys: client {_stats(c_ms)}; oracle "
            f"{_stats(o_ms)} (one client read costs ~{probe:.0f} ms of that)")

    # --- one step ----------------------------------------------------------

    def run_step(self, sc: Scenario, st: Step, idx: int, sdir: Path) -> list[StepResult]:
        target = st.acts[0].on if st.acts else "a"
        opening = bool(st.acts) and st.acts[0].kind == "open"
        if opening:
            target = str(st.acts[0].arg)
        before_c = (self._sig(self.sessions[target]) if target in self.sessions
                    else ("", -1))
        before_o = self._oracle_sig()
        self.step_notes = []
        t0 = time.monotonic()
        for a in st.acts:
            self._act(a, sc, sdir)
        client = self.sessions[target]
        changed_c, changed_o = [opening], [False]
        c_at: list[float | None] = [None]
        o_at: list[float | None] = [None]
        oracle_blind = all(c.client_only for c in st.until)

        quiet_sig = [(before_c, before_o)]
        quiet_since = [time.monotonic()]

        def still_probe() -> bool:
            now = time.monotonic()
            cur = (self._sig(client), self._oracle_sig())
            if cur != quiet_sig[0]:
                quiet_sig[0], quiet_since[0] = cur, now
                return False
            if now - quiet_since[0] < st.still:
                return False
            if c_at[0] is None and self._client_ok(st, client):
                c_at[0] = now - t0
            if o_at[0] is None and self._oracle_ok(st):
                o_at[0] = now - t0
            return c_at[0] is not None and o_at[0] is not None

        # A condition only counts once the screen has CHANGED since the
        # action: otherwise a "has X" left over from the previous step would
        # pass before this step's output even arrived.
        def probe() -> bool:
            if c_at[0] is None:
                # A held client never changes: nothing it is shown can.
                if not changed_c[0] and (client.dead() or self._sig(client) != before_c
                                         or self._held(client.grid())):
                    changed_c[0] = True
                if changed_c[0] and self._client_ok(st, client):
                    c_at[0] = time.monotonic() - t0
            if o_at[0] is None:
                if not changed_o[0] and self._oracle_sig() != before_o:
                    changed_o[0] = True
                if (changed_o[0] or oracle_blind) and self._oracle_ok(st):
                    o_at[0] = time.monotonic() - t0
            return c_at[0] is not None and o_at[0] is not None

        limit = st.timeout * self.scale
        wait_for(still_probe if st.still else probe, limit, POLL)
        settled = self._settle() if st.settle else True
        notes = list(self.step_notes)
        if c_at[0] is None:
            notes.append(f"the client never met {_conds(st)} within {limit:.0f}s")
        if o_at[0] is None:
            notes.append(f"the oracle never met {_conds(st)} within {limit:.0f}s")
        if not settled:
            notes.append(f"screen still changing after {SETTLE_MAX}s (captured anyway)")
        return [self._capture(sc, st, idx, sdir, name, c, c is client,
                              c_at[0] if c is client else None, o_at[0], settled,
                              "; ".join(notes))
                for name, c in self.sessions.items()]

    # What "changed since the action" compares: the screen text AND the bytes
    # received. A step can leave the text exactly as it was (``cls`` on a
    # screen that held only the prompt), and the text alone would then never
    # count as changed.
    @staticmethod
    def _sig(client: Client) -> tuple[str, int]:
        raw = getattr(client, "raw_log", None)
        size = raw.stat().st_size if isinstance(raw, Path) and raw.is_file() else -1
        return client.grid().text(), size

    def _oracle_sig(self) -> tuple[str, int]:
        text = self.oracle.grid().text()
        return text, int(getattr(self.oracle, "offset", -1))

    def _settle(self) -> bool:
        """Every live session AND the oracle unchanged for SETTLE_SECS: an
        action on one session repaints the others too."""
        def snap() -> list[object]:
            out: list[object] = [self.oracle.grid().text()]
            for c in self.sessions.values():
                if not c.dead():
                    g = c.grid()
                    out += [g.text(), g.cursor]
            return out

        last, stable_since = snap(), time.monotonic()
        deadline = time.monotonic() + SETTLE_MAX
        while time.monotonic() < deadline:
            time.sleep(POLL)
            cur = snap()
            if cur != last:
                last, stable_since = cur, time.monotonic()
            elif time.monotonic() - stable_since >= SETTLE_SECS:
                return True
        return False

    def _capture(self, sc: Scenario, st: Step, idx: int, sdir: Path, name: str,
                 client: Client, target: bool, c_secs: float | None, o_secs: float | None,
                 settled: bool, note: str) -> StepResult:
        suffix = "" if name == "a" else f".{name}"
        stem = sdir / f"{idx:02d}-{st.name}{suffix}"
        dead = client.dead()
        cg = client.grid()
        og = self.oracle.grid()
        files = client.save(stem)
        Path(f"{stem}.{self.oracle.tag}.txt").write_text(og.text() + "\n")
        files.append(f"{stem.name}.{self.oracle.tag}.txt")
        res = compare(og, cg, self.suite.known)
        body = report(res, og, cg, self.suite.known, (self.oracle.label, client.describe()))
        if dead:
            # Its screen is frozen at the moment it closed: nothing to compare.
            res = DiffResult(og.cols, og.rows, closed=True)
            body = "the client has EXITED (session closed): not compared\n"
        if note:
            body = f"note: {note}\n" + body
        Path(f"{stem}.diff.txt").write_text(body)
        files.append(f"{stem.name}.diff.txt")
        verdict = res.verdict()
        if self.shots or verdict not in OK_VERDICTS or note:
            shot: Path | None = None
            if self.picture is not None:
                shot = sdir / f"{stem.name}.{self.picture.tag}.png"
                try:
                    self.picture.shoot(shot, (og.cols, og.rows))
                    files.append(shot.name)
                except (GuestError, OSError) as e:
                    note = f"{note}; picture failed: {e}" if note else f"picture failed: {e}"
                    shot = None
            files += write_pictures(stem, shot, self.picture.label if self.picture else "",
                                    og, self.oracle.tag, self.oracle.label, cg, client.tag,
                                    f"{client.describe()} (session {name})", res)
        known = sorted({d.known for d in res.diffs if d.known})
        return StepResult(sc.name, st.name, name, verdict, res.char_diffs, res.colour_diffs,
                          res.unknown, known, c_secs, o_secs, settled, files, note, target,
                          res.fails())

    # --- one scenario ------------------------------------------------------

    def run_scenario(self, sc: Scenario) -> ScenarioResult:
        out = ScenarioResult(sc.name, sc.what)
        sdir = self.outdir / sc.name
        sdir.mkdir(parents=True, exist_ok=True)
        for old in sdir.iterdir():
            old.unlink()
        t0 = time.monotonic()
        cpu0 = qemu_cpu_secs(self.guest.state.qemu_pid)
        try:
            self.open_session(sc, "a", sdir)
            acts, until = self.suite.mark(f"cv-{sc.name}-{os.getpid()}")
            start = Step("start", (*self.suite.start, *acts), until)
            for i, st in enumerate((start, *sc.steps)):
                if self.heartbeat:
                    self.heartbeat.where = f"{sc.name}/{st.name}"
                rs = self.run_step(sc, st, i, sdir)
                out.steps += rs
                print(f"  {sc.name}/{st.name}: "
                      + ", ".join(f"[{r.session}] {r.verdict}"
                                  f" client={_secs(r.client_secs) if r.target else '-'}"
                                  f" oracle={_secs(r.oracle_secs)}" for r in rs), flush=True)
        except (GuestError, RuntimeError, KeyError) as e:
            out.error = str(e)
            print(f"  {sc.name}: ERROR {e}", flush=True)
        finally:
            for name, c in self.sessions.items():
                sent = getattr(c, "sent", None)
                if isinstance(sent, list):
                    (sdir / f"{name}.sent.txt").write_text(
                        "\n".join(str(x) for x in sent) + "\n")  # pyright: ignore[reportUnknownVariableType, reportUnknownArgumentType]
            self.close_sessions()
        out.secs = time.monotonic() - t0
        out.qemu_cores = (qemu_cpu_secs(self.guest.state.qemu_pid) - cpu0) / max(out.secs, 1e-3)
        return out


def _probe_cost_ms(client: Client) -> float:
    t0 = time.monotonic()
    for _ in range(5):
        client.grid()
    return (time.monotonic() - t0) * 200


def _conds(st: Step) -> str:
    return ", ".join(f"{c.kind}({c.arg})" if c.arg else c.kind for c in st.until)


def _stats(ms: list[float]) -> str:
    fin = sorted(v for v in ms if v != float("inf"))
    lost = len(ms) - len(fin)
    if not fin:
        return f"never echoed ({lost} keys)"
    tail = f", {lost} never echoed" if lost else ""
    return f"median {fin[len(fin) // 2]:.0f} ms, max {fin[-1]:.0f} ms{tail}"


def _secs(v: float | None) -> str:
    return "TIMEOUT" if v is None else f"{v:.2f}s"
