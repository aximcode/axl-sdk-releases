"""classes.py -- the classes file (test/pool/classes.toml): named machines.

The format is the file's own header comment. Every rule is checked when the
file loads, and a violation is a ClassError naming the file and the class:
a typo must never quietly become a different machine.
"""
from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from axlpool.klass import REPO, GuestSpec, mem_mb, mem_values


class ClassError(ValueError):
    """The classes file broke a rule; the message says which, exactly."""


@dataclass(frozen=True)
class GuestClass:
    """One resolved class: its own and inherited settings merged."""

    name: str
    args: tuple[str, ...]                    # every arch's, inheritance applied
    arch_args: dict[str, tuple[str, ...]]    # per arch, inheritance applied
    arches: tuple[str, ...]
    slots: int
    keep: str                                # "idle" | "lease"
    reset: str                               # "snapshot" | "reuse" | "reset"
    agent_latency: str                       # "always" | "window" | "firmware"


# Where a tree keeps its classes file.
CLASSES_FILE = Path("test") / "pool" / "classes.toml"
ARCHES = ("AARCH64", "X64")
KEEP = ("idle", "lease")
RESET = ("snapshot", "reuse", "reset")
CLASS_KEYS = ("agent_latency", "args", "arch", "arches", "extends", "keep", "reset", "slots")
LATENCY = ("always", "window", "firmware")
TOP_TABLES = ("class", "defaults")
# A class name names a CTest resource type verbatim (guest_<class>_<arch>,
# the CMake registration), and CTest takes [a-z_][a-z0-9_]* only.
CLASS_NAME = re.compile(r"[a-z][a-z0-9_]*")
DEFAULTS = {"slots": 4, "keep": "idle", "reset": "snapshot", "agent_latency": "always"}


def _strings(v: object, what: str) -> tuple[str, ...]:
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):  # pyright: ignore[reportUnknownVariableType]
        raise ClassError(f"{what} must be a list of strings")
    return tuple(cast("list[str]", v))


def _args(v: object, what: str) -> tuple[str, ...]:
    """A class's run-qemu.sh options: strings, none with a newline -- a lease
    hands them to run-qemu.sh one per line (AXL_POOL_CLASS_ARGS)."""
    args = _strings(v, what)
    for a in args:
        if "\n" in a:
            raise ClassError(f"{what}: an arg may not contain a newline -- got {a!r}")
    return args


class ClassSet:
    """Every class of one file."""

    def __init__(self, source: str, classes: dict[str, GuestClass], tree: Path) -> None:
        self.source = source
        self.tree = tree                     # whose run-qemu.sh, firmware and Shell make the machines
        self._classes = classes

    def get(self, name: str) -> GuestClass:
        if name not in self._classes:
            raise ClassError(f"{self.source}: no class `{name}` "
                             f"(known: {', '.join(self.names())})")
        return self._classes[name]

    def names(self) -> list[str]:
        return sorted(self._classes)

    def machine_args(self, name: str, arch: str) -> tuple[str, ...]:
        """The run-qemu.sh options of class @name on @arch: its args, then
        its arch's -- the machine spec() boots, and what a lease exports
        (AXL_POOL_CLASS_ARGS) for run-qemu.sh to compare a run with."""
        k = self.get(name)
        if arch not in k.arches:
            raise ClassError(f"{self.source}: class `{name}` is not for {arch} "
                             f"(arches: {', '.join(k.arches)})")
        return (*k.args, *k.arch_args.get(arch, ()))

    def spec(self, name: str, arch: str, agent: Path) -> GuestSpec:
        """The GuestSpec of class @name on @arch, running @agent."""
        return GuestSpec(arch, agent, self.machine_args(name, arch), self.get(name).agent_latency,
                         self.tree)


def loads(text: str, source: str, tree: Path = REPO) -> ClassSet:
    """Parse and check a classes file's @text; @source names it in errors,
    @tree is the checkout whose machines its classes are."""
    try:
        doc = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ClassError(f"{source}: not valid TOML: {e}") from None
    for top in doc:
        if top not in TOP_TABLES:
            raise ClassError(f"{source}: unknown table `{top}` (known: {', '.join(TOP_TABLES)})")
    defaults = dict(DEFAULTS)
    defaults.update(cast("dict[str, Any]", doc.get("defaults", {})))
    raw = cast("dict[str, dict[str, Any]]", doc.get("class", {}))
    if "none" in raw:
        raise ClassError(f"{source}: `none` is reserved (a test's guest=none means not pooled)")
    for name, body in raw.items():
        where = f"{source}: class `{name}`"
        if not CLASS_NAME.fullmatch(name):
            raise ClassError(f"{where}: a class name is lower-case letters, digits and underscores, "
                             "starting with a letter (it names a CTest resource type)")
        for key in body:
            if key not in CLASS_KEYS:
                raise ClassError(f"{where}: unknown key `{key}` (known: {', '.join(CLASS_KEYS)})")
        parent = body.get("extends")
        if parent is not None and parent not in raw:
            raise ClassError(f"{where} extends `{parent}`, which is not defined")

    resolved: dict[str, GuestClass] = {}

    def resolve(name: str, chain: list[str]) -> GuestClass:
        if name in resolved:
            return resolved[name]
        if name in chain:
            cycle = chain[chain.index(name):] + [name]
            raise ClassError(f"{source}: classes extend in a cycle: {' -> '.join(cycle)}")
        body = raw[name]
        where = f"{source}: class `{name}`"
        base = resolve(body["extends"], [*chain, name]) if "extends" in body else None
        args = (*(base.args if base else ()), *_args(body.get("args", []), f"{where}: args"))
        arch_args: dict[str, tuple[str, ...]] = dict(base.arch_args) if base else {}
        for arch, sub in cast("dict[str, dict[str, Any]]", body.get("arch", {})).items():
            if arch not in ARCHES:
                raise ClassError(f"{where}: unknown arch `{arch}` (known: {', '.join(ARCHES)})")
            arch_args[arch] = (*arch_args.get(arch, ()),
                               *_args(sub.get("args", []), f"{where}: arch.{arch}.args"))
        arches = _strings(body["arches"], f"{where}: arches") if "arches" in body \
            else (base.arches if base else ARCHES)
        for arch in arches:
            if arch not in ARCHES:
                raise ClassError(f"{where}: unknown arch `{arch}` (known: {', '.join(ARCHES)})")
        # The memory cap counts a guest by its --mem: one it cannot read would
        # be counted as the default, silently, and run-qemu.sh would refuse it
        # at every boot anyway -- so it is refused here, by run-qemu.sh's rule.
        for v in mem_values((*args, *(x for sub in arch_args.values() for x in sub))):
            if mem_mb(v) is None:
                raise ClassError(f"{where}: --mem `{v}` is not a size run-qemu.sh takes (<N>M or <N>G)")

        def pick(key: str) -> Any:
            if key in body:
                return body[key]
            return getattr(base, key) if base else defaults[key]

        slots, keep, reset = pick("slots"), pick("keep"), pick("reset")
        latency = pick("agent_latency")
        if latency not in LATENCY:
            raise ClassError(f"{where}: agent_latency must be one of {', '.join(LATENCY)} -- "
                             f"got `{latency}` (firmware for a class whose tests type on a USB "
                             "keyboard: a 1 ms tick loses keys)")
        if not isinstance(slots, int) or isinstance(slots, bool) or slots < 1:
            raise ClassError(f"{where}: slots must be an integer of at least 1 -- got `{slots}`")
        if keep not in KEEP:
            raise ClassError(f"{where}: keep must be one of {', '.join(KEEP)} -- got `{keep}`")
        if reset not in RESET:
            raise ClassError(f"{where}: reset must be one of {', '.join(RESET)} -- got `{reset}` "
                             "(a test that needs a fresh boot says guest=none, and is not pooled)")
        resolved[name] = GuestClass(name, args, arch_args, tuple(arches), slots, str(keep),
                                    str(reset), str(latency))
        return resolved[name]

    for name in raw:
        resolve(name, [])
    return ClassSet(source, resolved, tree)


def load(path: Path) -> ClassSet:
    """loads() of the file at @path, whose tree is where it sits:
    <tree>/test/pool/classes.toml."""
    return loads(path.read_text(), str(path), path.resolve().parents[2])
