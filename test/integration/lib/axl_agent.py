"""axl_agent.py -- a client for tools/agent.c's HTTP protocol.

The protocol is the header comment of tools/agent.c; this module only speaks
it. One keep-alive connection per AgentClient, reopened once if the guest
dropped it (a snapshot restore does). Used by test-agent-qemu.sh and by the
guest pool.
"""
from __future__ import annotations

import http.client
import json
import time

from dataclasses import dataclass
from typing import Any, cast

# The agent's keep-alive idle timeout is the HTTP server's default, 30 s; a
# connection idle longer than this is reopened BEFORE a request is sent.
IDLE_REOPEN_S = 20.0

# One probe()'s socket bound: an agent that is up answers /ping in
# milliseconds, so 2 s only ever waits on one that is not.
PROBE_TIMEOUT_S = 2.0


@dataclass(frozen=True)
class ExecResult:
    """One POST /exec answer. `http_status` != 200 means nothing ran and
    `error` holds the agent's text; otherwise the JSON fields."""

    http_status: int
    outcome: str = ""
    status: str = ""
    execute: str = ""
    reset_ok: bool = False
    ms: int = 0
    stdout: str | None = None
    stdout_state: str = ""
    stderr: str | None = None
    stderr_state: str = ""
    shell_exiting: bool = False
    canary: str = ""
    error: str = ""


class AgentClient:
    """Talks to one agent at host:port.

    After a snapshot restore or a reset of the guest, call close(): the
    keep-alive connection died with the old guest state, and a POST is never
    retried on a dead one (a GET is, once)."""

    def __init__(self, host: str, port: int, timeout: float = 60.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._conn: http.client.HTTPConnection | None = None
        self._last_use = 0.0

    def close(self) -> None:
        if self._conn is not None:
            self._conn.close()
            self._conn = None

    def _request(self, method: str, path: str, body: bytes | None = None,
                 retry: bool = True) -> tuple[int, bytes]:
        """One request; a dropped keep-alive connection is reopened once
        unless `retry` is false (a request that must not run twice)."""
        if self._conn is not None and time.monotonic() - self._last_use > IDLE_REOPEN_S:
            self.close()   # the server may have closed it: reopen while nothing is sent
        for attempt in range(2):
            if self._conn is None:
                self._conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
            try:
                headers = {"Content-Type": "text/plain; charset=utf-8"} if body is not None else {}
                self._conn.request(method, path, body=body, headers=headers)
                resp = self._conn.getresponse()
                data = resp.read()
                self._last_use = time.monotonic()
                return resp.status, data
            except (ConnectionError, http.client.HTTPException, OSError):
                self.close()
                if attempt == 1 or not retry:
                    raise
        raise AssertionError("unreachable")

    def wait_ready(self, deadline_s: float) -> None:
        """Poll /ping until it answers or the deadline passes. Each attempt
        is a probe(), so the deadline holds against a guest that accepts the
        connection and never answers."""
        end = time.monotonic() + deadline_s
        last: Exception | None = None  # what the last failed /ping raised
        while time.monotonic() < end:
            try:
                if self.probe(end - time.monotonic()) == "pong\n":
                    return
            except (OSError, http.client.HTTPException) as e:
                last = e
            time.sleep(0.2)
        raise TimeoutError(f"agent at {self.host}:{self.port} never answered /ping: {last}")

    def ping(self) -> str:
        _, body = self._request("GET", "/ping")
        return body.decode()

    def probe(self, budget_s: float, cap_s: float = PROBE_TIMEOUT_S) -> str:
        """One GET /ping on a fresh connection whose every socket wait is
        bounded by min(@cap_s, @budget_s), never retried, then
        closed: what a caller polling under its OWN deadline uses. ping()
        waits the client's timeout and retries once -- 2 x 60 s against a
        guest whose port forward accepts and whose agent never answers."""
        self.close()
        conn = http.client.HTTPConnection(self.host, self.port,
                                          timeout=max(0.05, min(cap_s, budget_s)))
        try:
            conn.request("GET", "/ping")
            return conn.getresponse().read().decode()
        finally:
            conn.close()

    def info(self) -> dict[str, object]:
        status, body = self._request("GET", "/info")
        if status != 200:
            raise RuntimeError(f"/info answered {status}: {body!r}")
        parsed = cast("dict[str, object]", json.loads(body))
        return parsed

    def raw(self, method: str, path: str, body: bytes | None = None) -> tuple[int, bytes]:
        """Any request, for tests of the protocol's own refusals. A POST is
        never retried (see exec)."""
        return self._request(method, path, body, retry=method != "POST")

    def exec(self, line: str | bytes, *, console: bool = False) -> ExecResult:
        """Run @line; @console: its output to the guest's console (serial),
        not captured -- kept even if the line crashes or hangs."""
        body = line.encode() if isinstance(line, str) else line
        # Never retried: a dropped connection may mean the line RAN (a
        # `reset` does exactly that), and running it twice is worse than
        # reporting the failure.
        path = "/exec?output=console" if console else "/exec"
        status, data = self._request("POST", path, body, retry=False)
        if status != 200:
            return ExecResult(http_status=status, error=data.decode(errors="replace"))
        j = cast("dict[str, Any]", json.loads(data))
        return ExecResult(http_status=200, outcome=str(j["outcome"]), status=str(j["status"]),
                          execute=str(j["execute"]), reset_ok=bool(j["reset_ok"]),
                          ms=int(j["ms"]), stdout=cast("str | None", j["stdout"]),
                          stdout_state=str(j["stdout_state"]),
                          stderr=cast("str | None", j["stderr"]),
                          stderr_state=str(j["stderr_state"]),
                          shell_exiting=bool(j["shell_exiting"]),
                          canary=str(j["canary"]))

    def quit(self) -> str:
        """Ask the agent to stop serving and return to the Shell."""
        _, body = self._request("POST", "/quit", b"", retry=False)
        self.close()
        return body.decode()
