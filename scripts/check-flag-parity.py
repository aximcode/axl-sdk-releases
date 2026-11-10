#!/usr/bin/env python3
"""check-flag-parity.py — the build paths must agree on what matters.

An AXL image can be produced three ways, and each carries its own copy of the
compile flags and the objcopy section list:

    the CMake build          the in-tree build (cmake/AxlFlags.cmake,
                             cmake/AxlImages.cmake, cmake/AxlWrapEfi.cmake)
    scripts/axl-cc           the SDK driver a consumer runs
    cmake/axl-config.cmake.in  the CMake package install() generates (find_package(axl))
                             -- which now DELEGATES to axl-cc, proved below

(The Makefile was a fourth until it was retired in 5.0.0.)

Copies drift, and this tree has the scar tissue to prove it. In one
afternoon both of these happened:

  * The stack protector was turned on in two build paths and NOT in the
    CMake package, so the same source built two ways had two different
    security postures depending on which path the consumer picked.

  * The aa64 linker script's relocation output section was renamed to
    `.rela.dyn` and `-j .rela.dyn` was added to two of the three objcopy lists.
    `objcopy -j` takes EXACT names, so the third path silently dropped EVERY
    relocation -- an image that boots into mis-relocated code.

Neither is visible in a diff of the file you are editing, which is what makes
them worth a gate rather than a review habit.

What is checked is deliberately narrow: flags where disagreement is a
correctness or security defect, not style. `-Os` vs `-O2` is a legitimate
difference between paths; `-fshort-wchar` is not, because it changes the ABI.

Comment lines are stripped first. A flag named only in a comment (axl-cc's
--help text mentions `-fno-stack-protector` as the opt-out) must not satisfy
the check.

THE PACKED-IMAGE WRAP is a second pair of paths: the CMake build's wrap
(cmake/AxlWrapEfi.cmake, every shipped tool) and `axl-cc --compress` (every
consumer). Each links the axl-pack stub with its own objcopy and runs
axl-efi-pack. Two things must agree between them, and are compared
explicitly:

  * the stub link's objcopy -j list. A stub link without `-j .axlpack` is an
    image with no payload, and nothing at build time says so.
  * axl-efi-pack's arguments: the same tool, given only --stub-size and -o.
    The wrap policy (threshold, filter, LZMA settings) lives in axl-efi-pack
    and nowhere else; a caller passing it a tuning flag forks the policy.

The wrap comparison RESOLVES shell variables and arrays (and the CMake
wrap's -j list through the SECTIONS it is handed), and an argument it cannot
resolve, or a stub link it cannot find, is a FAILURE: a gate that cannot see
the thing it compares must not report it clean. test-flag-parity-gate.sh
proves each way it can fail.

THE IMAGE LINK is a third comparison: the CMake build's AXL_LDFLAGS_EFI
(every in-tree image and the stub link) and axl-cc's LD_BASE_FLAGS (every
consumer image and its --compress stub). `-z noexecstack` reached one path
first and another a commit later; `--no-undefined` is what turns a misspelt
symbol into a link error rather than an image that jumps to address 0. Each
flag in CRITICAL_LD_FLAGS must be in both lists, as a parsed argument (`-z`
taken with its value), not a substring. Either list missing is a failure.

Usage: check-flag-parity.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Flags whose absence from ANY path is a defect. Each changes ABI, memory
# safety, or code generation in a way that makes two images incompatible or
# one of them unsafe.
CRITICAL_C_FLAGS = [
    "-ffreestanding",              # hosted libc headers would be wrong
    "-fshort-wchar",               # UEFI CHAR16; ABI-visible
    "-fno-builtin",                # gcc must not assume libc semantics
    "-fpic",                       # images are relocated at load
    "-fno-omit-frame-pointer",     # the RSOD unwinder walks the FP chain
    "-fstack-protector-strong",    # smash detection
    "-mstack-protector-guard=global",  # ... from a symbol, not %fs TLS
]

# Link flags both image links must pass. Each changes what the image IS or
# whether a defect is caught at link time; a difference between the paths is
# an image that links one way and not the other. (--fatal-warnings is
# in-tree only, deliberately: a consumer's link must not fail on a warning
# its own objects cause.)
CRITICAL_LD_FLAGS = [
    "-nostdlib",          # no host crt or libc in an image
    "-shared",            # a relocatable image the crt0 self-relocates
    "-Bsymbolic",         # references bind inside the image
    "--no-undefined",     # an unresolved symbol is a link error, not RIP 0
    "-z noexecstack",     # no executable-stack warning (fatal in-tree)
    "--gc-sections",      # dead code dropped; the image-size budget assumes it
]
CC_LD = "axl-cc LD_BASE_FLAGS"
CMAKE_LD = "CMake AXL_LDFLAGS_EFI"

PATHS = {
    "scripts/axl-cc": Path("scripts/axl-cc"),
    "cmake/axl-config.cmake.in": Path("cmake/axl-config.cmake.in"),
}

# THE CMAKE BUILD, the in-tree build: its compile flags are in AxlFlags.cmake, its image link's flags
# and objcopy -j list in AxlImages.cmake, and its packed-image wrap in
# AxlWrapEfi.cmake. The first two are read as ONE path ("CMake build"): the
# flags of one build split across two files.
CMAKE_PATH = "CMake build"
CMAKE_FILES = (Path("cmake/AxlFlags.cmake"), Path("cmake/AxlImages.cmake"))
CMAKE_WRAP_FILE = Path("cmake/AxlWrapEfi.cmake")
CMAKE_WRAP = "CMake wrap (AxlWrapEfi.cmake)"


# A trailing comment starts at whitespace-then-hash. `$#` and `${VAR#pat}`
# have no preceding whitespace, so shell parameter expansion survives.
TRAILING_COMMENT_RE = re.compile(r"\s#.*$")


def uncommented(path: Path) -> str:
    """@a path with comments removed, whole-line AND trailing.

    Shell and cmake both use `#`. A flag mentioned only in prose must not
    count as a flag that is passed — axl-cc's --help text names
    `-fno-stack-protector` as the opt-out, and an earlier version of this gate
    that stripped only whole-line comments accepted
    `FLAGS = -fno-stack-protector  # -fstack-protector-strong` as compliant.

    Over-stripping is the safe direction: it can only cause a false FAILURE,
    which is loud. Under-stripping causes a false PASS, which is the thing
    this gate exists to prevent.
    """
    keep: list[str] = []
    for line in path.read_text().splitlines():
        if line.lstrip().startswith("#"):
            continue
        keep.append(TRAILING_COMMENT_RE.sub("", line))
    return "\n".join(keep)


def objcopy_sections(text: str) -> set[str]:
    """Every literal `-j <section>` in @a text."""
    return set(re.findall(r"-j\s+(\.[A-Za-z0-9_.]+)", text))


# ---------------------------------------------------------------------------
# The packed-image wrap: the CMake build's wrap vs axl-cc --compress
# ---------------------------------------------------------------------------

CC_WRAP = "axl-cc --compress"
PACK_TOOL = "axl-efi-pack"
# The only arguments a caller passes axl-efi-pack, each taking a value.
PACK_ARGS = ("--stub-size", "-o")
SECTION_RE = re.compile(r"^\.[A-Za-z0-9_.]+$")
PACK_CMD_RE = re.compile(r"(?:^|[;&|]|\bthen\b|\belse\b|\bdo\b)[ \t]*\S*?"
                         r"(?<![A-Za-z0-9_-])axl-efi-pack(?![A-Za-z0-9_-])",
                         re.MULTILINE)


def join_continuations(text: str) -> str:
    """@a text with every backslash-newline continuation joined."""
    return re.sub(r"\\\n[ \t]*", " ", text)


def tokenize(cmd: str) -> list[str]:
    """Split a shell command into words.

    Whitespace separates words except inside quotes and inside a balanced
    `$(...)` / `${...}`, so `$$(stat -c%s 'x')` stays one word. A word stops
    at `||`, `&&` or `;` outside those, which ends the command.
    """
    words: list[str] = []
    cur = ""
    depth = 0
    quote = ""
    i = 0
    while i < len(cmd):
        c = cmd[i]
        if quote:
            cur += c
            if c == quote:
                quote = ""
        elif c in "'\"":
            quote = c
            cur += c
        elif c in "({":
            depth += 1
            cur += c
        elif c in ")}":
            depth = max(depth - 1, 0)
            cur += c
        elif depth == 0 and (cmd.startswith("||", i) or cmd.startswith("&&", i)
                             or c == ";"):
            break
        elif depth == 0 and c in " \t\n":
            if cur:
                words.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    if cur:
        words.append(cur)
    return words


def shell_arrays(text: str) -> dict[str, list[str]]:
    """Column-0 `NAME=( ... )` array literals (single definition only)."""
    out: dict[str, list[str]] = {}
    count: dict[str, int] = {}
    for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_]*)=\((.*?)\)", text,
                         re.MULTILINE | re.DOTALL):
        count[m.group(1)] = count.get(m.group(1), 0) + 1
        out[m.group(1)] = tokenize(join_continuations(m.group(2)))
    return {n: v for n, v in out.items() if count[n] == 1}


def shell_scalars(text: str) -> dict[str, str]:
    """Column-0 `NAME=value` scalar assignments (single definition only)."""
    out: dict[str, str] = {}
    count: dict[str, int] = {}
    for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_]*)=([^(\s][^\n]*)$", text,
                         re.MULTILINE):
        count[m.group(1)] = count.get(m.group(1), 0) + 1
        out[m.group(1)] = m.group(2).strip().strip('"').strip("'")
    return {n: v for n, v in out.items() if count[n] == 1}


ARRAY_REF_RE = re.compile(r'^"?\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}"?$')
SCALAR_REF_RE = re.compile(r'^"?\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?"?$')


def shell_resolve(words: list[str], arrays: dict[str, list[str]],
                  scalars: dict[str, str]) -> list[str]:
    """Splice array references and substitute scalar ones; an unresolvable
    reference is kept as written (and so stays visibly unresolved)."""
    out: list[str] = []
    for w in words:
        m = ARRAY_REF_RE.match(w)
        if m:
            out.extend(arrays.get(m.group(1), [w]))
            continue
        m = SCALAR_REF_RE.match(w)
        if m and m.group(1) in scalars:
            out.append(scalars[m.group(1)])
            continue
        out.append(w)
    return out


def j_sections(words: list[str], where: str,
               problems: list[str]) -> set[str]:
    """The `-j` values in @a words; an unresolved one is a problem."""
    found: set[str] = set()
    for i, w in enumerate(words):
        if w != "-j":
            continue
        val = words[i + 1] if i + 1 < len(words) else "<nothing>"
        if SECTION_RE.match(val):
            found.add(val)
        else:
            problems.append(f"wrap: {where}: -j {val} does not resolve to a "
                            f"section name")
    return found


def pack_flags(words: list[str], where: str,
               problems: list[str]) -> tuple[str, list[str]]:
    """(tool basename, flags) of an axl-efi-pack invocation's words."""
    if not words:
        return "", []
    tool = words[0].strip('"').rsplit("/", 1)[-1]
    flags: list[str] = []
    # The input image is identified by its place in the grammar -- the word
    # after -o's value -- not by being last: "an unresolved word at the end
    # must be the input" would wave through a trailing flag variable. Every
    # other word is a flag (with its value) or a failure.
    input_at = -1
    if "-o" in words:
        input_at = words.index("-o") + 2
    if input_at < 0 or input_at >= len(words):
        problems.append(f"wrap: {where}: no input image after -o's value")
    i = 1
    while i < len(words):
        w = words[i].strip('"')
        if i == input_at:
            pass
        elif w.startswith("-"):
            flags.append(w)
            if w in PACK_ARGS:
                i += 1          # its value
        elif w.startswith("$"):
            # An unresolved reference outside a value or the input slot could
            # carry anything.
            problems.append(f"wrap: {where}: axl-efi-pack argument {words[i]} "
                            f"does not resolve")
        else:
            problems.append(f"wrap: {where}: axl-efi-pack gets an unexpected "
                            f"argument {words[i]}")
        i += 1
    return tool, sorted(flags)


def shell_function(text: str, name: str) -> str | None:
    """The body of `name() {` ... the `}` at the same indentation, or None."""
    m = re.search(rf"^([ \t]*){re.escape(name)}\(\)[ \t]*\{{[ \t]*$", text,
                  re.MULTILINE)
    if not m:
        return None
    end = re.compile(rf"^{re.escape(m.group(1))}\}}[ \t]*$", re.MULTILINE)
    e = end.search(text, m.end())
    return text[m.end():e.start()] if e else None


def axlcc_wrap(text: str, problems: list[str]) -> tuple[set[str], str, list[str]] | None:
    """(stub -j set, packer tool, packer flags) of axl-cc --compress."""
    arrays = shell_arrays(text)
    scalars = shell_scalars(text)
    body = shell_function(text, "pack_stub_link")
    if body is None:
        problems.append(f"wrap: {CC_WRAP}: no pack_stub_link() function -- "
                        f"the stub link cannot be compared")
        return None
    joined = join_continuations(body)
    at = joined.find("objcopy")
    if at < 0:
        problems.append(f"wrap: {CC_WRAP}: pack_stub_link() runs no objcopy")
        return None
    stub = shell_resolve(tokenize(joined[at:]), arrays, scalars)
    sections = j_sections([w.strip('"') for w in stub], CC_WRAP, problems)

    calls = [m.start() for m in re.finditer(
        r'^[ \t]*(?:run_cmd[ \t]+)?"?\$\{?AXL_EFI_PACK\}?"?[ \t]',
        join_continuations(text), re.MULTILINE)]
    if len(calls) != 1:
        problems.append(f"wrap: {CC_WRAP}: {len(calls)} axl-efi-pack "
                        f"invocations (expected exactly one)")
        return sections, "", []
    line = join_continuations(text)[calls[0]:].lstrip()
    if line.startswith("run_cmd"):
        line = line[len("run_cmd"):].lstrip()
    words = shell_resolve(tokenize(line), arrays, scalars)
    return (sections, *pack_flags(words, CC_WRAP, problems))


def cmake_stub_sections(images_text: str, wrap_text: str,
                        problems: list[str]) -> set[str] | None:
    """The CMake wrap's stub -j set: AxlImages.cmake hands the wrap its image
    list as SECTIONS, and AxlWrapEfi.cmake adds its own literal -j's. Both
    halves are REQUIRED to be spelled that way; another spelling cannot be
    compared and is a failure, not a pass."""
    image = cmake_set(images_text, "AXL_OBJCOPY_SECTIONS")
    handed = re.search(r'"-DSECTIONS=\$<JOIN:\$\{AXL_OBJCOPY_SECTIONS\},\|>"',
                       images_text)
    used = re.search(r"\$\{OBJCOPY\}\s+\$\{SECTIONS\}", wrap_text)
    if image is None or handed is None or used is None:
        problems.append(f"wrap: {CMAKE_WRAP}: its stub's -j list is not "
                        f"AXL_OBJCOPY_SECTIONS handed in as SECTIONS -- the "
                        f"stub link cannot be compared")
        return None
    return j_sections(image, CMAKE_WRAP, problems) | objcopy_sections(wrap_text)


def wrap_parity(images_text: str, wrap_text: str, cc_text: str,
                problems: list[str]) -> str:
    """Compare the two wrap paths' stub links, and axl-cc's packer call (the
    CMake wrap's is cmake_wrap_parity's); returns the clean-summary
    fragment."""
    cm = cmake_stub_sections(images_text, wrap_text, problems)
    cc = axlcc_wrap(cc_text, problems)
    if cm is None or cc is None:
        return ""
    cc_sec, cc_tool, cc_flags = cc
    before = len(problems)
    if cm != cc_sec:
        only_cm = ", ".join(sorted(cm - cc_sec)) or "(none)"
        only_cc = ", ".join(sorted(cc_sec - cm)) or "(none)"
        problems.append(f"wrap: the axl-pack stub link's objcopy -j lists "
                        f"differ -- only in {CMAKE_WRAP}: {only_cm}; only in "
                        f"{CC_WRAP}: {only_cc}")
    if cc_tool and cc_tool != PACK_TOOL:
        problems.append(f"wrap: {CC_WRAP} runs {cc_tool}, not {PACK_TOOL}")
    extra = sorted(set(cc_flags) - set(PACK_ARGS))
    if extra:
        problems.append(f"wrap: {CC_WRAP}: passes {PACK_TOOL} "
                        f"{' '.join(extra)} -- the wrap policy lives in "
                        f"{PACK_TOOL}, not in a caller")
    elif sorted(set(cc_flags)) != sorted(PACK_ARGS):
        problems.append(f"wrap: {CC_WRAP}: the packer call lacks "
                        f"{' '.join(a for a in PACK_ARGS if a not in cc_flags)}")
    if len(problems) != before:
        return ""
    return (f"the axl-pack stub link ({len(cm)} objcopy sections) agrees "
            f"between {CMAKE_WRAP} and {CC_WRAP}, and {CC_WRAP} passes "
            f"{PACK_TOOL} only {' '.join(sorted(PACK_ARGS))}")


def ld_args(words: list[str]) -> set[str]:
    """@a words as link arguments: `-z` joined with its value (`-zfoo` and
    `-z foo` alike), every other word as written."""
    out: set[str] = set()
    i = 0
    while i < len(words):
        w = words[i].strip('"')
        if w == "-z" and i + 1 < len(words):
            value = words[i + 1].strip('"')
            out.add(f"-z {value}")
            i += 2
            continue
        if w.startswith("-z") and len(w) > 2:
            out.add(f"-z {w[2:]}")
        else:
            out.add(w)
        i += 1
    return out


def cmake_set(text: str, name: str) -> list[str] | None:
    """The words of `set(NAME word...)` in CMake @a text (comments already
    stripped), or None when there is no such set(). `${...}` references are
    kept as words: the flags compared here are all literal."""
    m = re.search(rf"\bset\(\s*{re.escape(name)}\s+([^)]*)\)", text)
    return m.group(1).split() if m else None


def ld_parity(cc_text: str, cmake_text: str, problems: list[str]) -> str:
    """Compare the image links' flags; returns the clean-summary fragment."""
    arrays = shell_arrays(cc_text)
    cm = cmake_set(cmake_text, "AXL_LDFLAGS_EFI")
    sides: dict[str, set[str] | None] = {
        CC_LD: (ld_args(arrays["LD_BASE_FLAGS"])
                if "LD_BASE_FLAGS" in arrays else None),
        CMAKE_LD: ld_args(cm) if cm is not None else None,
    }
    for where, have in sides.items():
        if have is None:
            problems.append(f"ld: {where}: not found -- the link flags cannot "
                            f"be compared")
    if any(have is None for have in sides.values()):
        return ""
    before = len(problems)
    for flag in CRITICAL_LD_FLAGS:
        absent = [w for w, have in sides.items() if have is not None and flag not in have]
        if len(absent) == len(sides):
            problems.append(f"ld: {flag}: MISSING from {', '.join(absent)}")
        elif absent:
            present = [w for w in sides if w not in absent]
            problems.append(f"ld: {flag}: in {', '.join(present)} but MISSING "
                            f"from {', '.join(absent)}")
    if len(problems) != before:
        return ""
    return (f"{len(CRITICAL_LD_FLAGS)} critical ld flags "
            f"({' '.join(CRITICAL_LD_FLAGS)}) agree between "
            f"{', '.join(sides)}")


def cmake_wrap_parity(text: str, problems: list[str]) -> str:
    """The CMake wrap's stub link must carry the payload section, and its
    packer call must pass only what every other caller passes: the stub size,
    the output, and the caller's own flags variable (--force for the test
    images). Returns the clean-summary fragment."""
    before = len(problems)
    if not re.search(r"-j\s+\.axlpack\b", text):
        problems.append(f"wrap: {CMAKE_WRAP}: the stub link does not carry "
                        f"`-j .axlpack` -- an image with no payload")
    calls = re.findall(r"COMMAND\s+\$\{PACK_TOOL\}([^)]*)\)", text)
    if len(calls) != 1:
        problems.append(f"wrap: {CMAKE_WRAP}: {len(calls)} packer call(s) "
                        f"found (want exactly one) -- the wrap cannot be compared")
    else:
        words = calls[0].split()
        # Every other reference is unresolvable here and could carry a
        # tuning flag, as axl-cc's side refuses one (pack_flags): only the
        # caller's ${PACK_FLAGS} (held below), the stub size, the output and
        # the input may be references.
        allowed_refs = {"${PACK_FLAGS}", "${_stub_size}", '"${STATE}.pack"',
                        '"${RAW}"'}
        for w in words:
            if "$" in w and w not in allowed_refs:
                problems.append(f"wrap: {CMAKE_WRAP}: axl-efi-pack argument "
                                f"{w} does not resolve")
        args = [w for w in words if w.startswith("-")]
        extra = [w for w in args if w not in PACK_ARGS]
        if extra:
            problems.append(f"wrap: {CMAKE_WRAP}: passes axl-efi-pack "
                            f"{' '.join(extra)} -- the wrap policy lives in "
                            f"axl-efi-pack, not in a caller")
        elif sorted(args) != sorted(PACK_ARGS):
            problems.append(f"wrap: {CMAKE_WRAP}: the packer call lacks "
                            f"{' '.join(a for a in PACK_ARGS if a not in args)}")
    # The references allowed above are only as good as what the script puts
    # in them: no command of its own may write one (a `list(APPEND
    # PACK_FLAGS --lc 0)` passed every check above). The input loop's
    # `string(REPLACE ... ${_v} ...)` names them only through ${_v}, and
    # `file(SIZE "${_stub}" _stub_size)` is the size's one writer.
    for cmd, args in re.findall(r"\b(set|list|string|math|separate_arguments|"
                                r"cmake_parse_arguments|file)\s*\(([^)]*)\)", text):
        if cmd == "file" and re.fullmatch(r'\s*SIZE\s+"\$\{_stub\}"\s+_stub_size\s*', args):
            continue
        for var in ("PACK_FLAGS", "_stub_size", "STATE", "RAW"):
            if re.search(rf"(?<![${{\w]){var}(?![\w}}])", args):
                problems.append(f"wrap: {CMAKE_WRAP}: {cmd}(...) writes {var} "
                                f"-- the packer's arguments must be what the "
                                f"caller handed in")
    return "" if len(problems) != before else f"{CMAKE_WRAP} agrees"


# What the image rules hand the wrap as PACK_FLAGS: --force for the test
# images that must wrap whatever the saving, else nothing. Anything else is a
# caller tuning the packer.
PACK_FLAGS_ARG = "-DPACK_FLAGS=$<$<BOOL:${A_FORCE}>:--force>"


def cmake_pack_flags_parity(images_text: str, problems: list[str]) -> None:
    """Every -DPACK_FLAGS= the CMake image rules pass is exactly
    PACK_FLAGS_ARG."""
    found = re.findall(r"-DPACK_FLAGS=\S*", images_text)
    if not found:
        problems.append(f"wrap: cmake/AxlImages.cmake hands the wrap no "
                        f"-DPACK_FLAGS= -- the packer's caller flags cannot be "
                        f"compared")
    for arg in found:
        if arg != PACK_FLAGS_ARG:
            problems.append(f"wrap: cmake/AxlImages.cmake hands the wrap "
                            f"{arg}; only {PACK_FLAGS_ARG} (--force, test "
                            f"images) is allowed")


def main(argv: list[str]) -> int:
    missing_files = [n for n, p in PATHS.items() if not p.exists()]
    missing_files += [str(p) for p in (*CMAKE_FILES, CMAKE_WRAP_FILE) if not p.exists()]
    if missing_files:
        print(f"check-flag-parity: FAIL — missing {', '.join(missing_files)}")
        return 1

    bodies = {name: uncommented(path) for name, path in PATHS.items()}
    bodies[CMAKE_PATH] = "\n".join(uncommented(p) for p in CMAKE_FILES)
    problems: list[str] = []

    # The wrap comparison reads the full files; the general -j comparison
    # below then sets the stub links aside -- the stub carries one section
    # (.axlpack) no ordinary image does, and the wrap check owns it.
    wrap_note = wrap_parity(uncommented(CMAKE_FILES[1]), uncommented(CMAKE_WRAP_FILE),
                            bodies["scripts/axl-cc"], problems)
    ld_note = ld_parity(bodies["scripts/axl-cc"], bodies[CMAKE_PATH], problems)
    cmake_wrap_note = cmake_wrap_parity(uncommented(CMAKE_WRAP_FILE), problems)
    cmake_pack_flags_parity(uncommented(CMAKE_FILES[1]), problems)
    stub_body = shell_function(bodies["scripts/axl-cc"], "pack_stub_link")
    if stub_body is not None:
        bodies["scripts/axl-cc"] = bodies["scripts/axl-cc"].replace(stub_body, "")

    # A path that DELEGATES to axl-cc cannot drift from it, so it is exempted
    # from the comparison -- but the delegation is PROVED, not assumed.
    #
    # The CMake package (generated by install.sh until it became the install()
    # template cmake/axl-config.cmake.in) used to reimplement axl-cc's pipeline in the
    # generated axl-config.cmake: its own compile line, ld, objcopy and
    # pe-set-debug, ~200 lines of it. That was the third path this gate exists
    # to police, and policing it was never as good as removing it -- a gate
    # comparing SPELLINGS could confirm both said `-j .eh_frame` and not that
    # the CMake package could build an exceptions image, which for a while it
    # could not.
    #
    # So the exemption is conditional on the marker below. Reintroduce a
    # hand-rolled compile or objcopy there and the file stops delegating; if it
    # then also lacks the flags, this fires exactly as it used to.
    # `COMMAND ${AXL_CC}`, not a bare mention of the variable. The file also
    # names it in a FATAL_ERROR string, and matching that made a sabotage
    # replacing BOTH invocations read as still-delegating -- the same "named
    # only in prose" trap this file's docstring warns about for
    # -fno-stack-protector.
    #
    # A path that stops delegating is NOT an error by itself: it simply rejoins
    # the comparison below and has to carry every flag itself, which is what it
    # did before this exemption existed. Failing outright would contradict this
    # gate's own message ("fix the one that drifted").
    delegated: list[str] = []
    for name, body in list(bodies.items()):
        if name == "cmake/axl-config.cmake.in" and "COMMAND ${AXL_CC}" in body:
            delegated.append(name)
            del bodies[name]

    for flag in CRITICAL_C_FLAGS:
        absent = [name for name, body in bodies.items() if flag not in body]
        if absent:
            present = [n for n in bodies if n not in absent]
            problems.append(
                f"{flag}: present in {', '.join(present) or 'nothing'} but "
                f"MISSING from {', '.join(absent)}")

    # objcopy -j sets. A section carried by one path and not another is a
    # silently different image, and .rela.dyn showed that can mean "no
    # relocations at all".
    sections = {name: objcopy_sections(body) for name, body in bodies.items()}
    union: set[str] = set()
    for s in sections.values():
        union |= s
    for name, have in sections.items():
        gap = union - have
        if gap:
            problems.append(
                f"objcopy -j: {name} does not carry {', '.join(sorted(gap))} "
                f"(other paths do). `-j` takes EXACT section names.")

    if problems:
        print("check-flag-parity: FAIL — the build paths disagree on flags that "
              "change ABI, safety or relocation:")
        for p in problems:
            print(f"  {p}")
        print("  Every path must produce an equivalent image. Fix the one that "
              "drifted, do not relax this list.")
        return 1

    note = ""
    if delegated:
        note = f"; {', '.join(delegated)} delegates to axl-cc"
    if wrap_note:
        note += f"; {wrap_note}"
    if ld_note:
        note += f"; {ld_note}"
    if cmake_wrap_note:
        note += f"; {cmake_wrap_note}"
    print(f"check-flag-parity: clean — {len(CRITICAL_C_FLAGS)} critical flags and "
          f"{len(union)} objcopy sections agree across {len(bodies)} build "
          f"path(s){note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
