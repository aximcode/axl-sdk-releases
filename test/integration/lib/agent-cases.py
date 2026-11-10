#!/usr/bin/env python3
"""agent-cases.py -- tools/agent.c's protocol, case by case, against a live guest.

Usage: agent-cases.py HOST PORT ARCH nonest|nested

Prints `  PASS: <case>` / `  FAIL: <case> ...` per case and a final
`agent-cases: <p> passed, <f> failed`; exit status 1 on any failure. Every
assertion is exact: a field compared whole, never a substring.
"""
from __future__ import annotations

import dataclasses
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from axl_agent import AgentClient, ExecResult  # noqa: E402
from checks import Cases  # noqa: E402


# A DEBUG AXL app ends with its leak verdict on stderr, timestamped; the
# harness's leak gate depends on it, so it stays, and is removed here as one
# exact whole line before the rest of stderr is compared.
VERDICT = re.compile(r"\d\d:\d\d:\d\d\.\d{6} \[INFO\]  mem: no leaks detected\r\n")


def app_err(r: ExecResult) -> ExecResult:
    """r with an AXL app's leak verdict line removed from its stderr."""
    if r.stderr is None:
        return r
    return dataclasses.replace(r, stderr=VERDICT.sub("", r.stderr, count=1))


CAP = "captured"

# big.txt, staged by test-agent-qemu.sh: UCS-2 with a BOM, an `x` then
# 524,300 U+00E9 -- 1,048,601 UTF-8 bytes, just over the cap, so the 1 MiB
# cut falls INSIDE a two-byte character and must step back to the one
# before it. Kept that small: `type` writing it through the Shell to the
# capture file is the slowest line in the suite under TCG.
BIG_E_COUNT = 524300
BIG = "x" + "\u00e9" * BIG_E_COUNT
STREAM_MAX = 1024 * 1024


def truncated_big() -> str:
    """The longest prefix of BIG whose UTF-8 fits in 1 MiB."""
    n = 0
    for i, ch in enumerate(BIG):
        n += len(ch.encode())
        if n > STREAM_MAX:
            return BIG[:i]
    return BIG
RED = "redirected"


def fields(r: ExecResult) -> tuple[object, ...]:
    return (r.http_status, r.outcome, r.status, r.stdout, r.stdout_state, r.stderr,
            r.stderr_state)


def ran(stdout: str | None, stderr: str | None = "", status: str = "0x0",
        out_state: str = CAP, err_state: str = CAP) -> tuple[object, ...]:
    return (200, "ran", status, stdout, out_state, stderr, err_state)


def nonest_cases(c: Cases, a: AgentClient, arch: str, serial: str) -> None:
    info = a.info()
    c.check("/info: protocol, arch, -nonest, 1 ms tick, port",
            (info.get("agent"), info.get("arch"), info.get("nonest"), info.get("tick_us"),
             info.get("port")),
            (2, arch, True, 1000, 8080))

    # ?output=console: no capture -- the line's output goes to the console
    # (the serial log), as it happens, so a line that crashes or hangs
    # still leaves everything it printed.
    r = a.exec("echo CONSOLE-MARK-7", console=True)
    c.check("exec ?output=console: ran, 0, both streams `console` (null)", fields(r),
            ran(None, None, out_state="console", err_state="console"))
    seen = False
    for _ in range(50):
        if b"CONSOLE-MARK-7" in Path(serial).read_bytes().replace(b"\x00", b""):
            seen = True
            break
        time.sleep(0.1)
    c.check("exec ?output=console: the output is on the serial log", seen, True)
    status, body = a.raw("POST", "/exec?output=bogus", b"echo x")
    c.check("exec with an unknown query: 400, nothing ran",
            (status, body), (400, b"agent: unknown query `output=bogus` (only output=console)\n"))

    r = a.exec("echo hi")
    c.check("echo: ran, 0, its stdout, empty stderr", fields(r), ran("hi\r\n"))
    c.check("echo: the status reset worked, a wall time is reported, the canary is ok",
            (r.reset_ok, r.execute, 0 <= r.ms < 10000, r.canary, r.shell_exiting),
            (True, "0x0", True, "ok", False))
    r = a.exec(r"fs0:\sexitb.nsh")
    c.check("a script's exit /b 3 ends only the script: ran 0x3, the Shell is not exiting",
            (r.http_status, r.outcome, r.status, r.shell_exiting), (200, "ran", "0x3", False))
    c.check("UTF-8 survives the UCS-2 capture", fields(a.exec("echo café")), ran("café\r\n"))
    c.check("start-up: a value-taking option given none",
            fields(app_err(a.exec(r"fs0:\agent.efi --port"))),
            ran("", "agent: --port needs a value\r\n", status="0x2"))
    c.check("start-up: a port with trailing junk",
            fields(app_err(a.exec(r"fs0:\agent.efi --port 8080x"))),
            ran("", "agent: bad port 8080x (1..65535)\r\n", status="0x2"))
    c.check("start-up: a capture directory that cannot be written (the probe)",
            fields(app_err(a.exec(r"fs0:\agent.efi --out fs0:\no-such-dir"))),
            ran("", "agent: the capture directory fs0:\\no-such-dir is not writable\r\n",
                status="0x2"))
    c.check("start-up: a rooted --out resolves on the current volume, then the probe",
            fields(app_err(a.exec(r"fs0:\agent.efi --out \no-such-dir"))),
            ran("", "agent: the capture directory \\no-such-dir is not writable\r\n",
                status="0x2"))
    c.check("start-up: a relative --out resolves against the current directory",
            fields(app_err(a.exec(r"fs0:\agent.efi --out no-such-dir"))),
            ran("", "agent: the capture directory no-such-dir is not writable\r\n",
                status="0x2"))
    # (`type` would print them as `.`: the line carries them to echo.)
    c.check("control characters are relayed, escaped in the JSON (ESC, U+0001)",
            fields(a.exec("echo a\x1b[0mb\x01c")), ran("a\x1b[0mb\x01c\r\n"))
    c.check("an app's status",
            fields(app_err(a.exec(r"fs0:\exit-status-selftest.efi 3"))),
            ran("exit-status-selftest: arming 0x3, returning rc=1\r\n", status="0x3"))
    c.check("an unknown command did not run",
            fields(a.exec("no-such-command")), (200, "not-run", "0xE", "", CAP, "", CAP))
    c.check("stderr is captured on its own",
            fields(app_err(a.exec(r"fs0:\agent.efi --bogus"))),
            ran("", "agent: unknown option --bogus\r\n", status="0x2"))

    a.exec("echo noisy")
    c.check("a silent line after a noisy one returns nothing, never the earlier output",
            fields(a.exec("cd .")), ran(""))
    c.check("a line redirecting stdout itself: stdout redirected",
            fields(a.exec(r"echo mine > fs0:\mine.txt")), ran(None, out_state=RED))
    c.check("... and its redirect was honoured (type adds a line end)",
            fields(a.exec(r"type fs0:\mine.txt")), ran("mine\r\n\r\n"))
    c.check("an appended-redirect spelling (>>a) is seen too",
            fields(a.exec(r"echo more >>a fs0:\asc.txt")), ran(None, out_state=RED))
    c.check("1> is a stdout redirect",
            fields(a.exec(r"echo a 1> fs0:\r1.txt")), ran(None, out_state=RED))
    c.check(">v (to a variable) is a stdout redirect",
            fields(a.exec("echo a >v AXLAGENTV")), ran(None, out_state=RED))
    c.check("2>> is a stderr redirect",
            fields(app_err(a.exec(r"fs0:\agent.efi --bogus 2>> fs0:\r2.txt"))),
            ran("", None, status="0x2", err_state=RED))
    c.check('a quoted ">" is not an operator',
            fields(a.exec('echo ">"')), ran(">\r\n"))
    c.check("an escaped ^> is not an operator",
            fields(a.exec("echo a ^> b")), ran("a > b\r\n"))
    c.check("a comment-only line: ran, nothing captured or placed",
            fields(a.exec("# only a comment")), ran(""))
    # The client owns the timeout (contract): this line is slow on AARCH64.
    slow = AgentClient(a.host, a.port, timeout=300.0)
    big = slow.exec(r"type fs0:\big.txt")
    slow.close()
    c.check("over 1 MiB: truncated, cut at a character boundary",
            (big.http_status, big.outcome, big.stdout_state, big.stdout == truncated_big()),
            (200, "ran", "truncated", True))
    c.check("a line redirecting stderr itself: stderr redirected, stdout captured",
            fields(app_err(a.exec(r"fs0:\agent.efi --bogus 2> fs0:\e.txt"))),
            ran("", None, status="0x2", err_state=RED))
    c.check("a comment does not swallow the capture",
            fields(a.exec("echo hi # a comment")), ran("hi\r\n"))
    c.check("an escaped # is not a comment",
            fields(a.exec("echo a^#b")), ran("a#b\r\n"))
    c.check("a pipe: the last segment's stdout is captured",
            fields(app_err(a.exec(r"echo hello | fs0:\exit-status-selftest.efi 5"))),
            ran("exit-status-selftest: arming 0x5, returning rc=1\r\n", status="0x5"))
    c.check("a stderr redirect in an EARLIER pipe segment leaves the last one captured",
            fields(app_err(a.exec(r"fs0:\agent.efi --bogus 2> fs0:\e2.txt | echo after"))),
            # 0x2 is the Shell's own report (measured): the builtin echo ending
            # the pipe leaves the first segment's %lasterror% in place.
            ran("after\r\n", status="0x2"))

    c.check("exit is refused, nothing run",
            (a.exec("exit").http_status, a.ping()), (403, "pong\n"))
    c.check("exit's refusal text, any spelling",
            a.exec('e"xit" 3').error,
            "agent: refused: exit would end the Shell the agent runs in\n")
    for name, body, reason in (
            ("empty line", b"", "an empty line"),
            ("blank line", b"  \t ", "a blank line"),
            ("a newline in the line", b"echo a\necho b", "a CR, LF or NUL in the line"),
            ("a CR in the line", b"echo a\r", "a CR, LF or NUL in the line"),
            ("a NUL in the line", b"echo a\x00b", "a CR, LF or NUL in the line"),
            ("invalid UTF-8", b"echo \xff\xfe", "invalid UTF-8"),
            ("4001 bytes", b"echo " + b"x" * 3996, "a line over 4000 bytes"),
            ("an unclosed quote", b'echo "abc', "an unclosed quote"),
            ("a quoted # is still a comment: unclosed as the Shell sees it",
             b'echo "a#b"', "an unclosed quote"),
            ("a final ^", b"echo a^", "a final ^ escapes nothing")):
        st, txt = a.raw("POST", "/exec", body)
        c.check(f"400: {name}", (st, txt.decode(errors="replace")), (400, f"agent: {reason}\n"))
    c.check("4000 bytes runs", fields(a.exec("echo " + "x" * 3995)), ran("x" * 3995 + "\r\n"))
    c.check("unknown path: 404", a.raw("GET", "/nope")[0], 404)
    c.check("still serving after all of it", a.ping(), "pong\n")

    # The capture directory's volume renamed under the agent (as removing a
    # hot-plugged USB stick renumbers volumes): it finds its directory again
    # by a marker file, rather than failing every later line.
    a.exec("map fs9 fs0:")
    a.exec("map -d fs0:")
    r = a.exec("echo moved")
    c.check("the capture volume renamed (fs0: unmapped, fs9: left): the agent follows it",
            (r.http_status, r.stdout, r.stdout_state), (200, "moved\r\n", CAP))
    a.exec("map fs0 fs9:")
    a.exec("map -d fs9:")
    r = a.exec("echo back")
    c.check("... and back again", (r.http_status, r.stdout), (200, "back\r\n"))

    # The canary must never mistake user state for a poisoned Shell: each of
    # these once read as one and refused every later line forever.
    a.exec("alias cd echo")
    r = a.exec("echo x")
    # echo writes its own status, so it reads RAN though the reset was skipped.
    c.check("an alias named cd (status reset skipped): canary ok, not exiting",
            (r.outcome, r.reset_ok, r.canary, r.shell_exiting), ("ran", False, "ok", False))
    a.exec("alias -d cd")
    r = a.exec(r"rm -q fs0:\*.canary.nsh")
    c.check("the canary script deleted by a line: rewritten, ok, not exiting",
            (r.canary, r.shell_exiting), ("ok", False))
    a.exec("alias set echo")
    r = a.exec("echo y")
    # Measured: an alias does not reach the canary script's built-in `set`.
    c.check("an alias named set does not reach the canary's built-in: ok, not exiting",
            (r.stdout, r.canary, r.shell_exiting), ("y\r\n", "ok", False))
    a.exec("alias -d set")

    # LAST: an `exit` inside a script poisons the Shell (measured: later
    # scripts are cut short reporting RAN 0, and the Shell exits when the
    # agent returns). The agent's canary sees it; nothing after it may
    # report a result.
    # The poisoning script turns nonesting off first: the canary must still
    # be run in the agent's own Shell, or the exit would go unseen.
    r = a.exec(r"fs0:\snest.nsh")
    c.check("an exit inside a script, after `set nonesting False`: this answer says the Shell is exiting",
            (r.http_status, r.outcome, r.canary, r.shell_exiting), (200, "ran", "poisoned", True))
    st, txt = a.raw("POST", "/exec", b"echo after")
    c.check("... and every later line is refused, nothing run",
            (st, txt.decode()),
            (409, "agent: the Shell is exiting (an exit ran): restore or reset the machine\n"))
    c.check("... /info says so", a.info().get("shell_exiting"), True)
    c.check("/quit answers before the agent stops", a.quit(), "bye\n")


def nested_cases(c: Cases, a: AgentClient, arch: str) -> None:
    info = a.info()
    c.check("/info under a nesting Shell: nonest false",
            (info.get("agent"), info.get("arch"), info.get("nonest")), (2, arch, False))
    r = a.exec("echo hi")
    c.check("a nesting Shell cannot say whether a line ran: unknown, output still captured",
            (r.http_status, r.outcome, r.stdout, r.stdout_state, r.canary),
            (200, "unknown", "hi\r\n", CAP, "skipped"))
    r = a.exec("no-such-command")
    c.check("nested: a line the Shell never ran left no capture file: lost, not empty",
            (r.http_status, r.outcome, r.stdout, r.stdout_state, r.stderr, r.stderr_state),
            (200, "unknown", None, "lost", None, "lost"))
    r = app_err(a.exec(r"fs0:\agent.efi --bogus"))
    c.check("nested: an app's status and stderr are still reported, outcome unknown",
            (r.http_status, r.outcome, r.status, r.stdout, r.stderr),
            (200, "unknown", "0x2", "", "agent: unknown option --bogus\r\n"))
    c.check("nested: /quit answers", a.quit(), "bye\n")


def main() -> int:
    host, port, arch, mode, serial = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
    c = Cases()
    a = AgentClient(host, port)
    try:
        a.wait_ready(120.0)
        if mode == "nonest":
            nonest_cases(c, a, arch, serial)
        else:
            nested_cases(c, a, arch)
    except Exception as e:  # a dead agent is a failure, reported, never a crash
        print(f"  FAIL: agent ({mode}) stopped answering: {type(e).__name__}: {e}")
        c.failed += 1
    finally:
        a.close()
    print(f"agent-cases: {c.passed} passed, {c.failed} failed")
    return 0 if c.failed == 0 and c.passed > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
