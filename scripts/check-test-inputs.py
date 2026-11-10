#!/usr/bin/env python3
"""check-test-inputs.py -- every tree file a test hands its guest is in the
skip cache's key, or says why it is not.

The skip cache (test/integration/lib/test-cache.sh) skips a test whose inputs
match its last green run, and "a cached green means the tests actually ran
against your code" holds only while every file the guest is given is IN the
key. A test that copies a tree file into $TEST_STAGING, or `--mount`s an
in-repo directory, without that file reaching the key is a FALSE HIT waiting
for an edit: the test reads green without running. Two reviews found ten such
sites by hand (09147d6b); this finds the next one.

WHAT IS IN THE KEY, by the key's own readers -- this asks them rather than
keeping a second copy:
  - a tree file the test (or a test it names) spells as a literal
    `$PROJECT_DIR/...` path, directly or through a variable set from one
    (`test-cache.sh --literals`);
  - a product of the test's declared `builds=` targets, from the build graph
    (`build-products.sh --tree`);
  - a file the test RECORDS: an argument of test_record_input,
    cache_record_input or test_add_efi in the same file;
  - the harness (test/integration's non-test files): always hashed.

THE RULE. A copy (cp, install, ln, rsync, mv), a redirect or a tee into
$TEST_STAGING -- or into a directory the file passes to `--mount` -- whose
source is a tree file, and every `--mount` of an in-repo path, must have its
source in the key as above, or carry the marker on the line or the comment
lines directly above it:

    # test-input: not-keyed -- <why>

A directory is never keyed (it may hold thousands of files), so a `--mount`
of one always needs the marker, which states the decision where it is made.

Scanned: test/integration/test-*.sh, common-test.sh and lib/*.sh (a lib has no
literals or builds= of its own: there, only a record or the marker covers a
copy). Not scanned: scripts/run-qemu.sh, which records what it stages itself.

A command is found behind a keyword or prefix (`then cp`, `timeout 5 cp`),
`--mount=PATH` is a mount, and `dd if=` a source. A bare relative path that
names a tree file (after a `cd`) is a source no literal covers.

WHAT IT CANNOT SEE, said rather than hidden: a source with a leading
expansion it cannot resolve to a root (a function argument, `$(...)`, an
array, a variable no scanned file sets) is not judged but COUNTED as
unresolved in the clean line, beside the "not tree files" (plain text, a
mktemp) and the lines it could not parse; `-v` lists every verdict. Not
seen at all: a write whose destination is a loop's redirect (`done > $F`)
or a pipe into `tar -C`. It fails outright when the key's readers cannot
run, and when it finds no staging write at all -- a parser that sees
nothing would report clean forever.

Usage: scripts/check-test-inputs.py --tree BUILD_TREE [--root DIR]
  --tree  a configured CMake tree (its build.ninja names the builds= products)
  --root  the source tree to scan (default: this checkout; a fixture's in the
          gate's own test)
Exit: 0 clean; 1 an unkeyed input, or the check could not run.
"""
from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

MARKER = "test-input: not-keyed"
# The marker as a comment WITH its reason: `# test-input: not-keyed -- <why>`.
# Text inside a string, or a marker with no reason, is not one.
MARKER_RE = re.compile(r"(^|\s)#\s*test-input: not-keyed --\s*\S")
COPY_CMDS = {"cp", "install", "ln", "rsync", "mv"}
RECORDERS = {"test_record_input": 0, "cache_record_input": 1, "test_add_efi": 0}
SEPARATORS = {";", "&&", "||", "|", "&", "(", ")", "{", "}"}
# Words that start a command without being it: keywords, and prefixes that
# run the rest (`timeout N`, `flock FILE` take one argument).
LEADERS = {"then", "do", "else", "elif", "if", "while", "until", "!", "command", "exec",
           "time", "nohup", "env", "sudo"}
LEADERS_WITH_ARG = {"timeout", "flock"}
# Options of the leaders that take a VALUE (timeout -k 5, flock -w 3, env -u X).
LEADER_OPTS_WITH_VALUE = {"timeout": {"-k", "-s", "--kill-after", "--signal"},
                          "flock": {"-w", "-E", "--timeout", "--conflict-exit-code"},
                          "env": {"-u", "-C", "-S", "--unset", "--chdir", "--split-string"}}
REDIRECTS = {">", ">>", ">|", "&>", "&>>"}
STAGING_ROOTS = ("$TEST_STAGING",)
# Roots a source or destination is classified BY: never expanded further
# (common-test.sh assigns TEST_STAGING a mktemp, which would hide it).
ROOT_VARS = {"PROJECT_DIR", "TEST_STAGING", "SCRIPT_DIR", "TESTS_DIR"}
ASSIGN_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?:[:#%/][^}]*)?\}|\$([A-Za-z_][A-Za-z0-9_]*)")
HEREDOC_RE = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")
BUILD_SUB_RE = re.compile(r"^\$\((?:test_build_dir|\"?\$PROJECT_DIR/scripts/build-prefix\.sh\"?)[^)]*\)/(.*)$")
BUILD_PREFIX_RE = re.compile(r"^\$PROJECT_DIR/\$\(test_build_prefix[^)]*\)/(.*)$")
TREE_BUILD_RE = re.compile(r"^\$PROJECT_DIR/build/[^/]+/(.*)$")
HARNESS_RE = re.compile(r"^\$(?:SCRIPT_DIR|TESTS_DIR)/")
MAX_EXPANSIONS = 32
MAX_DEPTH = 6


@dataclass
class Line:
    """One logical line: continuations joined, its first physical line number."""
    no: int
    text: str
    marked: bool


@dataclass
class Site:
    """One staged source: where, what was written, and what it resolves to."""
    path: Path
    no: int
    word: str
    mount: bool
    marked: bool


@dataclass
class Scan:
    """What one file says: its logical lines, assignments, records, mounts."""
    lines: list[Line] = field(default_factory=lambda: [])
    assigns: dict[str, list[str]] = field(default_factory=lambda: {})
    records: list[str] = field(default_factory=lambda: [])
    mount_dirs: list[str] = field(default_factory=lambda: [])


def normalize(word: str) -> str:
    """`${VAR}` and `${VAR:-x}` as `$VAR`, so one spelling is compared."""
    return VAR_RE.sub(lambda m: "$" + (m.group(1) or m.group(2)), word)


def logical_lines(text: str) -> list[Line]:
    """Continuations joined; heredoc bodies dropped (a fixture script written
    into a file is not this file's code); `marked` when the line or the
    comment block directly above it carries MARKER."""
    out: list[Line] = []
    phys = text.split("\n")
    i = 0
    pending_mark = False
    while i < len(phys):
        raw = phys[i]
        stripped = raw.strip()
        if stripped.startswith("#"):
            pending_mark = pending_mark or bool(MARKER_RE.search(stripped))
            i += 1
            continue
        if not stripped:
            pending_mark = False
            i += 1
            continue
        no = i + 1
        buf = raw
        while buf.endswith("\\") and i + 1 < len(phys):
            i += 1
            buf = buf[:-1] + " " + phys[i]
        i += 1
        # A quoted string spanning lines (a fixture script held in a
        # variable) is ONE word, not code: join until the quotes balance.
        joined = 0
        while tokens(buf) is None and i < len(phys) and joined < 400:
            buf += "\n" + phys[i]
            i += 1
            joined += 1
        out.append(Line(no, buf, pending_mark or bool(MARKER_RE.search(trailing_comment(buf)))))
        pending_mark = False
        m = HEREDOC_RE.search(buf.replace("<<<", "   "))
        if m:
            end = m.group(2)
            while i < len(phys) and phys[i].strip() != end:
                i += 1
            i += 1
    return out


def trailing_comment(text: str) -> str:
    """The comment part of @text (from its comment's `#`), or ""."""
    code = strip_comment(text)
    return text[len(code):] if text.startswith(code) else ""


def strip_comment(text: str) -> str:
    """@text without a trailing shell comment: a `#` that starts a word,
    outside quotes. (shlex's own commenters also cut `${x#y}` mid-word.)"""
    quote = ""
    prev = " "
    out: list[str] = []
    k = 0
    while k < len(text):
        c = text[k]
        if quote:
            if c == "\\" and quote == '"':
                out.append(text[k:k + 2])
                k += 2
                prev = "x"
                continue
            if c == quote:
                quote = ""
        elif c == "\\":
            out.append(text[k:k + 2])
            k += 2
            prev = "x"
            continue
        elif c in "'\"":
            quote = c
        elif c == "#" and prev in " \t\n;|&(":
            nl = text.find("\n", k)          # a joined line resumes after it
            if nl < 0:
                break
            k = nl
            continue
        out.append(c)
        prev = c
        k += 1
    return "".join(out)


def tokens(text: str) -> list[str] | None:
    """Shell words, quotes removed, `$VAR` kept; None when it cannot parse."""
    lex = shlex.shlex(strip_comment(text), posix=True, punctuation_chars=";&|()<>")
    lex.whitespace_split = True
    lex.commenters = ""
    try:
        return list(lex)
    except ValueError:
        return None


def commands(toks: list[str]) -> list[list[list[str]]]:
    """Pipelines of simple commands: [[cmd, ...], ...] per pipeline."""
    pipes: list[list[list[str]]] = [[[]]]
    for t in toks:
        if t == "|":
            pipes[-1].append([])
        elif t in SEPARATORS or t in {";;", "&&", "||"}:
            pipes.append([[]])
        else:
            pipes[-1][-1].append(t)
    return [[c for c in p if c] for p in pipes if any(p)]


def scan_file(path: Path) -> tuple[Scan, list[int]]:
    """Its lines, assignments, recorded words and --mount words; and how many
    lines it could not parse (their numbers)."""
    s = Scan(lines=logical_lines(path.read_text(errors="replace")))
    unparsed: list[int] = []
    for ln in s.lines:
        toks = tokens(ln.text)
        if toks is None:
            unparsed.append(ln.no)
            continue
        for pipe in commands(toks):
            for cmd in pipe:
                words: list[str] = list(cmd)
                while words and words[0] in {"local", "export", "readonly", "declare", "typeset"}:
                    words = [w for w in words[1:] if not w.startswith("-")]
                for w in words:
                    m = ASSIGN_RE.match(w)
                    if not m:
                        break
                    s.assigns.setdefault(m.group(1), []).append(normalize(m.group(2)))
                if len(words) >= 3 and words[0] == "for" and words[2] == "in":
                    s.assigns.setdefault(words[1], []).extend(normalize(w) for w in words[3:])
                if words and words[0] in RECORDERS:
                    s.records.extend(normalize(w) for w in words[1 + RECORDERS[words[0]]:])
                for k, w in enumerate(words):
                    if w == "--mount" and k + 1 < len(words):
                        s.mount_dirs.append(normalize(words[k + 1]))
                    elif w.startswith("--mount="):
                        s.mount_dirs.append(normalize(w[len("--mount="):]))
    return s, unparsed


def expand(word: str, assigns: dict[str, list[str]], depth: int = 0) -> set[str]:
    """Every spelling of @word with its LEADING variable replaced by what the
    file assigns it, recursively -- the root is what classifies a source."""
    word = normalize(word)
    m = re.match(r"^\$([A-Za-z_][A-Za-z0-9_]*)(.*)$", word, re.S)
    if depth >= MAX_DEPTH or not m or m.group(1) not in assigns or m.group(1) in ROOT_VARS:
        return {word}
    out: set[str] = set()
    for value in assigns[m.group(1)]:
        for e in expand(value + m.group(2), assigns, depth + 1):
            out.add(e)
            if len(out) >= MAX_EXPANSIONS:
                return out
    return out


def is_staging(word: str, scan: Scan) -> bool:
    """Does @word name a place inside the guest's staging (or a mounted dir)?"""
    roots = list(STAGING_ROOTS) + [d for d in scan.mount_dirs if d.startswith("$")]
    for e in expand(word, scan.assigns) | {normalize(word)}:
        if any(e == r or e.startswith(r + "/") for r in roots):
            return True
    return False


def staged_sources(scan: Scan, path: Path) -> list[Site]:
    """The words a staging write reads, and every --mount word."""
    out: list[Site] = []
    for ln in scan.lines:
        toks = tokens(ln.text)
        if toks is None:
            continue
        for pipe in commands(toks):
            pipe_writes = False
            pipe_words: list[str] = []
            for cmd in pipe:
                words = [w for w in cmd if not ASSIGN_RE.match(w)] if cmd and ASSIGN_RE.match(cmd[0]) else cmd
                operands: list[str] = []
                writes = False
                k = 0
                while k < len(words):
                    w = words[k]
                    if w in REDIRECTS or w == "<":
                        if k + 1 < len(words):
                            if w != "<" and is_staging(words[k + 1], scan):
                                writes = True
                            elif w == "<":
                                operands.append(words[k + 1])
                        k += 2
                        continue
                    if w == "--mount" and k + 1 < len(words):
                        out.append(Site(path, ln.no, words[k + 1], True, ln.marked))
                        k += 2
                        continue
                    if w.startswith("--mount="):
                        out.append(Site(path, ln.no, w[len("--mount="):], True, ln.marked))
                        k += 1
                        continue
                    operands.append(w)
                    k += 1
                while operands and (operands[0] in LEADERS or operands[0] in LEADERS_WITH_ARG
                                    or ASSIGN_RE.match(operands[0])):
                    lead, operands = operands[0], operands[1:]
                    # Its options first (with their values), then -- for
                    # timeout/flock -- its one positional: `timeout -k 5 10
                    # cp`, `flock -x L cp`, `env -u X cp`.
                    takes = LEADER_OPTS_WITH_VALUE.get(lead, set())
                    while operands and operands[0].startswith("-"):
                        operands = operands[2:] if operands[0] in takes else operands[1:]
                    if lead in LEADERS_WITH_ARG and operands:
                        operands = operands[1:]
                if not operands:
                    continue
                name, args = operands[0], [a for a in operands[1:] if not a.startswith("-")]
                if name == "dd":
                    ifs = [a[3:] for a in args if a.startswith("if=")]
                    if any(a.startswith("of=") and is_staging(a[3:], scan) for a in args):
                        out.extend(Site(path, ln.no, a, False, ln.marked) for a in ifs)
                    continue
                if name in COPY_CMDS and any(is_staging(a, scan) for a in args):
                    out.extend(Site(path, ln.no, a, False, ln.marked)
                               for a in args if not is_staging(a, scan))
                    continue
                if name == "tee" and any(is_staging(a, scan) for a in args):
                    writes = True
                pipe_words.extend(a for a in args if not is_staging(a, scan))
                pipe_writes = pipe_writes or writes
            if pipe_writes:
                out.extend(Site(path, ln.no, a, False, ln.marked) for a in pipe_words)
    return out


def classify(word: str, scan: Scan, root: Path) -> tuple[str, set[str]]:
    """('source', relpaths) a source-tree file; ('build', patterns) under a
    build tree; ('harness', ...) under test/integration; ('none', ...) not a
    tree file -- text with no leading expansion that names no file under
    @root, or a mktemp; ('unresolved', ...) any other leading expansion (a
    variable no file assigns, a function argument, `$(...)`, an array):
    NOT judged, and counted, so the gate's blindness shows."""
    exps = expand(word, scan.assigns)
    kinds: dict[str, set[str]] = {}
    for e in exps:
        if not e.startswith("$"):
            # A bare relative path that names a tree file (after a
            # `cd "$PROJECT_DIR"`) is one the key cannot see.
            rel = e.rstrip("/")
            if "/" in rel and not rel.startswith("/") and ".." not in rel.split("/") \
                    and (root / rel).is_file():
                kinds.setdefault("source", set()).add(rel)
            else:
                kinds.setdefault("none", set()).add(e)
            continue
        if e.startswith("$(mktemp"):
            kinds.setdefault("none", set()).add(e)
            continue
        m = BUILD_SUB_RE.match(e) or BUILD_PREFIX_RE.match(e) or TREE_BUILD_RE.match(e)
        if m:
            kinds.setdefault("build", set()).add(m.group(1))
        elif e.startswith("$PROJECT_DIR/"):
            rel = e[len("$PROJECT_DIR/"):].rstrip("/")
            top = rel.split("/", 1)[0]
            kinds.setdefault("stage" if top in {"stage", "out"} else "source", set()).add(rel)
        elif HARNESS_RE.match(e):
            kinds.setdefault("harness", set()).add(e)
        else:
            kinds.setdefault("unresolved", set()).add(e)
    for k in ("source", "build", "stage", "unresolved", "harness", "none"):
        if k in kinds:
            return k, kinds[k]
    return "none", set()


EXPANSION_RE = re.compile(r"\$\([^)]*\)|\$\{[^}]*\}|\$[A-Za-z_0-9@*#?]+")


def product_re(rel: str) -> re.Pattern[str] | None:
    """What @rel can name in the build tree, its expansions each one path
    component at most (`[^/]*`: fnmatch's `*` also crosses `/`); None when
    its file name is an expansion alone -- that names any product at all."""
    parts = EXPANSION_RE.split(rel)
    if EXPANSION_RE.fullmatch(rel.rsplit("/", 1)[-1]):
        return None
    return re.compile("[^/]*".join(re.escape(x) for x in parts))


def judge(site: Site, kind: str, vals: set[str], is_test: bool, recorded: set[str],
          scan: Scan, literals: set[str], products: list[str]) -> str:
    """How @site's source reaches the key: a counts key, "none" (not a tree
    file), or "" (it does not)."""
    if kind in {"none", "unresolved"} or (kind == "harness" and not site.mount):
        return kind               # (a harness FILE is hashed; a mounted dir never is)
    if site.marked:
        return "marked"
    if site.mount:
        return ""                    # a directory: only the marker covers it
    # By literal path and builds= first, so the counts say what the key
    # reads on its own, then by record.
    if is_test and kind == "source" and all(v in literals for v in vals):
        return "literal"
    pats = [product_re(v) for v in vals] if kind == "build" else []
    if is_test and kind == "build" and all(
            pat is not None and any(pat.fullmatch(p) for p in products) for pat in pats):
        return "builds"
    if (expand(site.word, scan.assigns) | {normalize(site.word)}) & recorded:
        return "recorded"
    return ""


def helper(cmd: list[str]) -> list[tuple[str, str]]:
    """`TEST<TAB>VALUE` lines from a key reader; SystemExit when it cannot run."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as e:
        raise SystemExit(f"check-test-inputs: FAIL -- could not run {cmd[1]}: {e}") from None
    if r.returncode != 0:
        raise SystemExit(f"check-test-inputs: FAIL -- {' '.join(cmd[1:3])} could not answer "
                         f"(exit {r.returncode}):\n{r.stderr.strip()}")
    pairs: list[tuple[str, str]] = []
    for line in r.stdout.splitlines():
        t, _, v = line.partition("\t")
        if t and v:
            pairs.append((t, v))
    return pairs


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--tree", required=True, type=Path)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every staged source and how it was judged")
    ns = ap.parse_args()
    root: Path = ns.root.resolve()
    tree: Path = ns.tree.resolve()
    it = root / "test" / "integration"
    tests = sorted(it.glob("test-*.sh"))
    libs = [p for p in [it / "common-test.sh", *sorted((it / "lib").glob("*.sh"))] if p.is_file()]
    if not tests or not (tree / "build.ninja").is_file():
        print(f"check-test-inputs: FAIL -- nothing to check: {len(tests)} test(s) under {it}, "
              f"{'a' if (tree / 'build.ninja').is_file() else 'no'} build.ninja in {tree}")
        return 1
    names = [t.name for t in tests]
    literals: dict[str, set[str]] = {}
    for t, p in helper(["bash", str(it / "lib" / "test-cache.sh"), "--literals", *names]):
        literals.setdefault(t, set()).add(p)
    products: dict[str, list[str]] = {}
    for t, p in helper(["bash", str(it / "lib" / "build-products.sh"), "--tree", str(tree),
                        "X64", *[str(x) for x in tests]]):
        rel = p[len(str(tree)) + 1:] if p.startswith(f"{tree}/") else p
        products.setdefault(Path(t).name, []).append(rel)

    problems: list[str] = []
    counts = {"writes": 0, "literal": 0, "builds": 0, "recorded": 0, "marked": 0,
              "harness": 0, "none": 0, "unresolved": 0, "unparsed": 0}
    # What the libs assign (TEST_BUILD_DIR, TEST_TMPDIR, ...) resolves a
    # variable a file uses but does not set itself; its own assignment wins.
    lib_assigns: dict[str, list[str]] = {}
    for path in libs:
        for k, v in scan_file(path)[0].assigns.items():
            lib_assigns.setdefault(k, []).extend(v)
    for path in [*tests, *libs]:
        scan, unparsed = scan_file(path)
        scan.assigns = {**lib_assigns, **scan.assigns}
        counts["unparsed"] += len(unparsed)
        if ns.verbose:
            for no in unparsed:
                print(f"  {path.relative_to(root)}:{no}: UNPARSED (not judged)")
        is_test = path in tests
        recorded: set[str] = set()
        for w in scan.records:
            recorded |= expand(w, scan.assigns) | {normalize(w)}
        for site in staged_sources(scan, path):
            counts["writes"] += 1
            kind, vals = classify(site.word, scan, root)
            where = f"{path.relative_to(root)}:{site.no}"
            verdict = judge(site, kind, vals, is_test, recorded, scan,
                            literals.get(path.name, set()), products.get(path.name, []))
            if ns.verbose:
                print(f"  {where}: {'mount' if site.mount else 'copy'} {site.word} [{kind}] "
                      f"-> {verdict or 'UNKEYED'}")
            if verdict in counts:
                counts[verdict] += 1
            if verdict:
                continue
            what = "a --mount of" if site.mount else "a copy into the guest of"
            why = {"source": "not a literal path to a file the key reads",
                   "build": "not a product of this test's builds=" if is_test
                            else "a build output a lib stages without recording it",
                   "stage": "a staged-SDK file nothing keys"}.get(kind, "not in the key")
            if site.mount:
                why = "a directory is never keyed"
            problems.append(f"{where}: {what} {site.word} -- {why}")

    if counts["writes"] == 0:
        print("check-test-inputs: FAIL -- found no staging write at all in "
              f"{len(tests)} tests and {len(libs)} libs. The parser is blind; a gate that")
        print("  inspects nothing reports clean forever.")
        return 1
    if problems:
        print("check-test-inputs: FAIL -- a tree file reaches the guest outside the skip-cache key:")
        for p in problems:
            print(f"    {p}")
        print()
        print("  Edit that file and a cached run reads green without running the test.")
        print("  Name it to the key: a literal \"$PROJECT_DIR/<file>\" path, a builds= target")
        print("  that produces it, or test_record_input <file> where it is staged. If it is")
        print("  deliberately not keyed, say why on the line above:")
        print(f"      # {MARKER} -- <why>")
        return 1
    print(f"check-test-inputs: clean -- {counts['writes']} staged source(s) in {len(tests)} tests "
          f"and {len(libs)} libs: {counts['literal']} keyed by literal path, {counts['builds']} "
          f"by builds=, {counts['recorded']} recorded, {counts['harness']} harness, "
          f"{counts['marked']} marked not-keyed, {counts['none']} not tree files; "
          f"{counts['unresolved']} unresolved "
          f"and {counts['unparsed']} unparsed line(s) not judged.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
