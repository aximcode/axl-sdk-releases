"""The HTML gallery of a run: timing, the known-differences table, and per
step the side-by-side, the 3-up diff, the grid diff and the verdict."""
from __future__ import annotations

import html
from pathlib import Path

from cvlib.cases import Suite
from cvlib.runner import OK_VERDICTS, ScenarioResult


def _secs(v: float | None) -> str:
    return "TIMEOUT" if v is None else f"{v:.2f}s"


CSS = """
:root{--bg:#fafafa;--fg:#1b1b1b;--card:#fff;--line:#ddd;--ok:#1a7f37;--bad:#c62828;--warn:#9a6700}
@media (prefers-color-scheme: dark){:root{--bg:#141414;--fg:#e6e6e6;--card:#1e1e1e;--line:#333;
--ok:#4cc26b;--bad:#ff6b6b;--warn:#e3b341}}
body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;margin:0;padding:16px}
h1{font-size:20px}h2{font-size:17px;margin-top:28px;border-bottom:1px solid var(--line)}
table{border-collapse:collapse}td,th{border:1px solid var(--line);padding:3px 8px;text-align:left}
.step{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:10px;margin:10px 0}
.MATCH,.CLOSED{color:var(--ok);font-weight:600}.DIFF-TEXT,.ERROR,.TIMEOUT{color:var(--bad);font-weight:600}
.DIFF-COLOUR,.DIFF-CURSOR{color:var(--warn);font-weight:600}
img{max-width:100%;border:1px solid var(--line);margin:4px 0}
pre{background:var(--card);border:1px solid var(--line);padding:8px;overflow-x:auto;font-size:12px}
"""


def write_gallery(outdir: Path, suite: Suite, results: list[ScenarioResult],
                  timing: dict[str, float], meta: dict[str, str]) -> Path:
    """index.html: a self-contained gallery (relative images) of one run."""
    h: list[str] = ["<!doctype html><html><head><meta charset='utf-8'>",
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>",
                    f"<title>{html.escape(suite.name)} visual run</title><style>", CSS, "</style></head><body>",
                    f"<h1>{html.escape(suite.name)}: visual / differential run</h1>",
                    f"<p>{html.escape(suite.what)}</p>",
                    "<p>" + html.escape(" | ".join(f"{k}: {v}" for k, v in meta.items())) + "</p>",
                    "<table><tr><th>scenario</th><th>secs</th><th>qemu cores</th><th>steps</th>"
                    "<th>match</th><th>differ</th><th>fail</th></tr>"]
    for r in results:
        n_ok = sum(1 for s in r.steps if s.verdict in OK_VERDICTS)
        h.append(f"<tr><td><a href='#{r.name}'>{r.name}</a></td><td>{r.secs:.1f}</td>"
                 f"<td>{r.qemu_cores:.2f}</td>"
                 f"<td>{len(r.steps)}</td><td>{n_ok}</td><td>{len(r.steps) - n_ok}</td>"
                 f"<td>{sum(1 for s in r.steps if s.fails)}"
                 f"{' ERROR' if r.error else ''}</td></tr>")
    h.append("</table><p>" + html.escape(", ".join(f"{k} {v:.1f}s" for k, v in timing.items()))
             + "</p>")
    h.append("<h2>known differences</h2><table><tr><th>id</th><th>owner</th><th>what</th></tr>")
    for k in suite.known:
        h.append(f"<tr><td>{k.kid}</td><td>{k.owner}</td><td>{html.escape(k.what)}</td></tr>")
    h.append("</table>")
    for r in results:
        h.append(f"<h2 id='{r.name}'>{r.name} &mdash; {html.escape(r.what)} "
                 f"({r.secs:.1f}s)</h2>")
        if r.error:
            h.append(f"<p class='ERROR'>ERROR: {html.escape(r.error)}</p>")
        for s in r.steps:
            sd = f"{r.name}/"
            h.append(f"<div class='step'><b>{html.escape(s.step)}</b> [session {s.session}] "
                     f"<span class='{s.verdict}'>{s.verdict}</span> chars {s.char_diffs}, "
                     f"colours {s.colour_diffs}, unclassified {s.unknown}"
                     f"{', known ' + ','.join(s.known) if s.known else ''}; "
                     f"client {_secs(s.client_secs) if s.target else '-'}, "
                     f"oracle {_secs(s.oracle_secs)}")
            if s.note:
                h.append(f"<br><span class='TIMEOUT'>{html.escape(s.note)}</span>")
            for f in s.files:
                if f.endswith((".side.png", ".diff3.png")):
                    h.append(f"<br><a href='{sd}{f}'><img loading='lazy' src='{sd}{f}'></a>")
            diff = next((f for f in s.files if f.endswith(".diff.txt")), "")
            if diff:
                txt = (outdir / r.name / diff).read_text()
                h.append(f"<details><summary>{diff}</summary><pre>{html.escape(txt)}</pre>"
                         "</details>")
            h.append(" ".join(f"<a href='{sd}{f}'>{f.split('.', 1)[1]}</a>" for f in s.files
                              if f.endswith(".txt")))
            h.append("</div>")
    h.append("</body></html>")
    idx = outdir / "index.html"
    idx.write_text("\n".join(h))
    return idx
