"""resource_spec.py -- the CTest resource spec of a tree's guest classes:
`python3 -m axlpool resource-spec --tree T` (service.py's CTEST).

CTest is the pool's allocator (design doc §6): the spec gives each class
and arch a resource type, `guest_<class>_<arch>`, with the class's slots;
a pooled test asks for one (RESOURCE_GROUPS "guest_standard_x64:1") and
CTest never runs more of them at once than the pool will keep. The format
is CTest's own, version 1.0.
"""
from __future__ import annotations

import re

from axlpool.classes import ARCHES, ClassSet

ARCH_SUFFIX = {"X64": "x64", "AARCH64": "aa64"}
assert set(ARCH_SUFFIX) == set(ARCHES), "a new arch needs its CTest resource suffix"
_NOT_CTEST = re.compile(r"[^a-z0-9_]")     # CTest resource types: [a-z_][a-z0-9_]*


class SpecError(ValueError):
    """The spec cannot be written for these classes."""


def resource_type(class_name: str, arch: str) -> str:
    """The CTest resource type of @class_name on @arch."""
    return f"guest_{_NOT_CTEST.sub('_', class_name.lower())}_{ARCH_SUFFIX[arch]}"


def spec(classes: ClassSet) -> dict[str, object]:
    """The resource spec (CTest format 1.0) of @classes. Raises SpecError
    when two classes would share a resource type."""
    types: dict[str, list[dict[str, object]]] = {}
    owner: dict[str, str] = {}
    for name in sorted(classes.names()):
        k = classes.get(name)
        for arch in k.arches:
            rt = resource_type(name, arch)
            if rt in owner:
                raise SpecError(f"{classes.source}: classes {owner[rt]} and {name} both become "
                                f"CTest resource {rt}")
            owner[rt] = name
            types[rt] = [{"id": "0", "slots": k.slots}]
    return {"version": {"major": 1, "minor": 0}, "local": [types]}
