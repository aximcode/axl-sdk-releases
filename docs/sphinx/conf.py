"""Sphinx configuration for AXL SDK documentation."""
from __future__ import annotations

import os
from pathlib import Path

# -- Project info ------------------------------------------------------------

project = "AXL"
author = "AximCode"
copyright = "2025-2026, AximCode"  # noqa: A001
# Pull the release string from the canonical VERSION file at the repo
# root so docs stay in sync with the bumped version automatically.
# scripts/bump-version.sh is the only thing that should modify
# VERSION (it also keeps include/axl/axl-version.h in lockstep).
_repo_root = Path(__file__).resolve().parent.parent.parent
release = (_repo_root / "VERSION").read_text(encoding="utf-8").strip()
version = release

# -- Extensions --------------------------------------------------------------

extensions = [
    "breathe",
    "myst_parser",
]

# -- MyST (Markdown) ---------------------------------------------------------

myst_heading_anchors = 3
suppress_warnings = [
    "myst.header",
    "myst.xref_missing",
    # Breathe re-emits each anonymous-enum enumerator at the parent
    # enum's "line", so Sphinx's C domain sees them as duplicate
    # declarations. axl-smbios.h has 4 anonymous enums (table
    # types, IPMI iface, host iface, host iface protocol) — that's
    # where all 100+ "Duplicate C declaration" warnings come from.
    # The underlying source has no real duplicates; this is purely
    # a Breathe rendering artifact for unnamed enums.
    "duplicate_declaration.c",
]
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# -- Breathe -----------------------------------------------------------------

breathe_projects = {
    "axl": os.path.join(os.path.dirname(__file__), "..", "..", "out", "docs", "doxygen-xml"),
}
breathe_default_project = "axl"
breathe_default_members = ("members",)
breathe_domain_by_extension = {"h": "c"}
breathe_domain_by_file_pattern = {"*.h": "c"}

# -- HTML output -------------------------------------------------------------

html_theme = "sphinx_rtd_theme"
html_theme_options = {
    "navigation_depth": 3,
    "collapse_navigation": False,
}
# sphinx_rtd_theme caps content at ~800 px by default. Override via
# a tiny stylesheet so wide monitors actually use the screen.
html_static_path = ["_static"]
html_css_files   = ["axl.css"]
html_search_language = "en"

# -- Man pages ---------------------------------------------------------------

man_pages = [
    ("modules/mem",    "axl-mem",    "AXL memory allocation",       [author], 3),
    ("modules/format", "axl-format", "AXL printf engine",           [author], 3),
    ("modules/str",    "axl-str",    "AXL string utilities",        [author], 3),
    ("modules/string", "axl-string", "AXL string builder",          [author], 3),
    ("modules/stream",     "axl-stream",     "AXL stream / filesystem",             [author], 3),
    ("modules/log",    "axl-log",    "AXL logging",                [author], 3),
    ("modules/data",   "axl-data",   "AXL data structures",        [author], 3),
    ("modules/json",   "axl-json",   "AXL JSON parser/builder",    [author], 3),
    ("modules/cache",  "axl-cache",  "AXL TTL cache",              [author], 3),
    ("modules/config", "axl-config", "AXL configuration framework", [author], 3),
    ("modules/path",   "axl-path",   "AXL path manipulation",      [author], 3),
    ("modules/loop",   "axl-loop",   "AXL event loop",             [author], 3),
    ("modules/task",   "axl-task",   "AXL task pool and arena",    [author], 3),
    ("modules/net",    "axl-net",    "AXL networking",             [author], 3),
    ("modules/tls",    "axl-tls",   "AXL TLS support",            [author], 3),
    ("modules/sys",    "axl-sys",    "AXL system utilities",       [author], 3),
    ("modules/gfx",    "axl-gfx",   "AXL graphics",               [author], 3),
]
