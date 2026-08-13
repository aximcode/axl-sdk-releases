#!/usr/bin/env python3
"""Generate UEFI headers from spec HTML using a manifest.

Reads a manifest (uefi-manifest.json5) listing the specific types,
structs, enums, and defines that AXL needs.  For each entry, searches
the spec HTML <pre> blocks for a matching definition, extracts it by
its C boundaries, and emits it into the target header file.

The manifest order IS the dependency order — no sorting needed.

Usage:
    python3 scripts/generate-uefi-headers.py [--input DIR] [--output DIR]
    python3 scripts/generate-uefi-headers.py --dump /tmp/blocks.txt
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Callable
from html.parser import HTMLParser
from pathlib import Path

VALID_KINDS = {"struct", "union", "enum", "funcptr", "define", "typedef", "table"}


# ===================================================================
# JSON5-lite loader — strips // and /* */ comments and trailing
# commas so the manifest can carry inline "why" notes. Preserves
# string literals verbatim, so commas and comment-like sequences
# inside "..." are left alone.
# ===================================================================

def _strip_json5(text: str) -> str:
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]

        # String literal: copy through verbatim, honoring \ escapes.
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                ci = text[i]
                out.append(ci)
                i += 1
                if ci == '\\' and i < n:
                    out.append(text[i])
                    i += 1
                elif ci == '"':
                    break
            continue

        # Line comment: skip to end of line, keep the newline.
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i += 2
            while i < n and text[i] != '\n':
                i += 1
            continue

        # Block comment: skip, but preserve newlines so JSON parse
        # errors still point at the right line.
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                if text[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
            continue

        # Trailing comma: drop `,` when the next non-whitespace char
        # is `}` or `]`.
        if c == ',':
            j = i + 1
            while j < n and text[j] in ' \t\r\n':
                j += 1
            if j < n and text[j] in '}]':
                i += 1
                continue

        out.append(c)
        i += 1
    return ''.join(out)


def load_json5(path: Path) -> object:
    return json.loads(_strip_json5(path.read_text()))


# ===================================================================
# HTML parsers
# ===================================================================

class PreExtractor(HTMLParser):
    """Extract text content from <pre> blocks."""

    def __init__(self) -> None:
        super().__init__()
        self.in_pre = False
        self.text = ""
        self.blocks: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "pre":
            self.in_pre = True
            self.text = ""

    def handle_endtag(self, tag: str) -> None:
        if tag == "pre" and self.in_pre:
            self.in_pre = False
            t = self.text.strip()
            if t:
                self.blocks.append(t)

    def handle_data(self, data: str) -> None:
        if self.in_pre:
            self.text += data

    def handle_entityref(self, name: str) -> None:
        if self.in_pre:
            entities = {"amp": "&", "lt": "<", "gt": ">", "quot": '"'}
            self.text += entities.get(name, f"&{name};")

    def handle_charref(self, name: str) -> None:
        if self.in_pre:
            try:
                self.text += chr(int(name[1:], 16) if name.startswith("x")
                                 else int(name))
            except ValueError:
                self.text += f"&#{name};"


class TableCellExtractor(HTMLParser):
    """Extract text content from <td> cells."""

    def __init__(self) -> None:
        super().__init__()
        self.in_td = False
        self.text = ""
        self.cells: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "td":
            self.in_td = True
            self.text = ""

    def handle_endtag(self, tag: str) -> None:
        if tag == "td" and self.in_td:
            self.in_td = False
            self.cells.append(self.text.strip())

    def handle_data(self, data: str) -> None:
        if self.in_td:
            self.text += data


# ===================================================================
# Table 2-4 type parser (scalar/opaque types from prose table)
# ===================================================================

# Maps spec description phrases to C types
TABLE_TYPE_MAP: dict[str, str] = {
    "Type VOID *":   "void *",
    "Type VOID*":    "void *",
    "Type UINTN":    "UINTN",
    "Type UINT64":   "UINT64",
    "Type UINT32":   "UINT32",
    "Type UINT16":   "UINT16",
    "Type UINT8":    "UINT8",
    "Type INT32":    "INT32",
}

# Maps spec type names to stdint C types (for scalar types)
SCALAR_TYPE_MAP: dict[str, str] = {
    "BOOLEAN":  "uint8_t",
    "INT8":     "int8_t",
    "INT16":    "int16_t",
    "INT32":    "int32_t",
    "INT64":    "int64_t",
    "UINT8":    "uint8_t",
    "UINT16":   "uint16_t",
    "UINT32":   "uint32_t",
    "UINT64":   "uint64_t",
    "CHAR8":    "char",
    "CHAR16":   "uint16_t",
    "UINTN":    "uint64_t",
    "INTN":     "int64_t",
}


def parse_type_table(input_dir: Path) -> dict[str, str]:
    """Parse UEFI type table (Table 2-4) from Overview chapter.

    Returns a dict mapping type name -> C typedef string.
    """
    overview = input_dir / "02_Overview.html"
    if not overview.exists():
        return {}

    parser = TableCellExtractor()
    parser.feed(overview.read_text(encoding="utf-8", errors="replace"))

    result: dict[str, str] = {}
    i = 0
    while i < len(parser.cells) - 1:
        name = parser.cells[i].strip()
        desc = parser.cells[i + 1].strip()
        i += 2

        if not name or not name[0].isupper():
            continue

        for phrase, ctype in TABLE_TYPE_MAP.items():
            if phrase in desc:
                result[name] = f"typedef {ctype} {name};"
                break
        else:
            if name in SCALAR_TYPE_MAP:
                result[name] = f"typedef {SCALAR_TYPE_MAP[name]}  {name};"

    return result


def find_table_type(name: str, table_types: dict[str, str]) -> str | None:
    """Look up a type from the parsed Table 2-4."""
    return table_types.get(name)


# ===================================================================
# Text cleaning (RST/Sphinx artifact removal)
# ===================================================================

def clean_text(text: str) -> str:
    """Clean RST/Sphinx artifacts from extracted code."""
    # RST escape sequences
    text = text.replace("\\_", "_")
    text = text.replace("\\*", "*")
    text = text.replace("\\|", "|")
    text = text.replace("\\\\\n", "\\\n")
    text = text.replace("\u2026", "...")

    # Fix known spec typos
    text = text.replace("EFI_GRAPHICS_OUTPUT_PROTCOL", "EFI_GRAPHICS_OUTPUT_PROTOCOL")
    text = text.replace("EFI_SIMPLE_FILE_SYSTEM PROTOCOL", "EFI_SIMPLE_FILE_SYSTEM_PROTOCOL")
    text = text.replace("EFI_System_Table", "EFI_SYSTEM_TABLE")
    text = text.replace("EFI_IPv4_Address", "EFI_IPv4_ADDRESS")
    text = text.replace("EFI_DEVICE_PATH ", "EFI_DEVICE_PATH_PROTOCOL ")
    text = re.sub(r"\bEFI__HANDLE\b", "EFI_HANDLE", text)
    # Spec has EFI_IMAGE_UNLOAD for LoadImage field — should be EFI_IMAGE_LOAD
    text = re.sub(r"EFI_IMAGE_UNLOAD(\s+LoadImage)", r"EFI_IMAGE_LOAD\1", text)
    # Spec types the InstallMultipleProtocolInterfaces field with the
    # UNINSTALL funcptr (identical signature, but wrong) — should be INSTALL
    text = re.sub(
        r"EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES(\s+InstallMultipleProtocolInterfaces)",
        r"EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES\1", text)
    text = re.sub(r"}\s*FI_DHCP4_PACKET\s*;", "} EFI_DHCP4_PACKET;", text)

    # Fix missing space: "typedef struct_NAME" → "typedef struct _NAME"
    text = re.sub(r"typedef\s+struct_", "typedef struct _", text)
    # Fix missing space: "typedef struct{" → "typedef struct {"
    text = re.sub(r"typedef\s+struct\{", "typedef struct {", text)

    # Strip #pragma pack lines (we handle packing via struct attributes if needed)
    text = re.sub(r"#pragma\s+pack\([^)]*\)\s*\n?", "", text)
    # Strip RST bold/italic markers: *Name* → Name
    # Full-line: *typedef UINT8 EFI_SMBIOS_TYPE;*
    text = re.sub(r"^\*(.+)\*$", r"\1", text, flags=re.MULTILINE)
    # Inline: parameter names like *This* , *Handle* , *Type*
    text = re.sub(r"(?<=[\s(,])\*(\w+)\*(?=[\s),;])", r"\1", text)
    # RST bold on IN scalar params: "IN UINTN *Name," → "IN UINTN Name,"
    # A scalar IN parameter is never a pointer — the * is RST bold markup.
    text = re.sub(
        r"(IN\s+(?:UINTN|INTN|UINT\d+|INT\d+|BOOLEAN|EFI_TPL|"
        r"EFI_STATUS|EFI_SMBIOS_TYPE|EFI_SMBIOS_HANDLE)\s+)\*(\w+)",
        r"\1\2", text)

    # Pointer + bold: **Name* → *Name, ***Name,* → **Name,
    text = re.sub(r"\*\*\*(\w+),\*", r"**\1,", text)
    text = re.sub(r"\*\*(\w+)\*(?=[\s),;,])", r"*\1", text)

    # OPTIONAL parameter decoration fixes
    text = re.sub(r",\s*OPTIONAL\s*,", " OPTIONAL,", text)
    text = re.sub(r",\s*OPTIONAL\s*\n", " OPTIONAL,\n", text)
    text = re.sub(r"(OPTIONAL)\s*\n(\s+(?:IN |OUT ))", r"\1,\n\2", text)

    # Missing comma between function parameters on separate lines
    # (param line not ending with , or { followed by IN/OUT line)
    text = re.sub(r"(\w)\s*\n(\s+(?:IN |OUT )(?!.*\.\.\.))",
                  r"\1,\n\2", text)

    # Trailing comma before closing );
    text = re.sub(r",\s*\n(\s*\);)", r"\n\1", text)
    # Fix broken define: #define NAME(EXPR) VALUE → #define NAME VALUE
    text = re.sub(r"^(#define\s+\w+)\([^)]*\|[^)]*\)\s+(0x\w+)$",
                  r"\1  \2", text, flags=re.MULTILINE)

    # EFIAPI function pointer syntax fixes
    text = re.sub(r"(\(EFIAPI\s*\*\s*\w+\s*\))\s*\(\(", r"\1 (", text)
    text = re.sub(r"(\(EFIAPI\s*\*\s*\w+\s*\))\s*\n(\s+IN )", r"\1 (\n\2", text)
    text = re.sub(r"(?<!\()EFIAPI\s+\\?\*(\w+)\)", r"(EFIAPI *\1)", text)
    text = re.sub(r"\(\*\s+EFIAPI\s+(\w+)\)", r"(EFIAPI *\1)", text)

    return text


# ===================================================================
# Extractors — one per manifest kind
# ===================================================================

def find_struct(name: str, blocks: list[str]) -> str | None:
    """Find 'typedef struct ... { ... } NAME;' in pre blocks.

    Searches backwards from the closing '} NAME;' to find the nearest
    matching 'typedef struct', handling multi-definition blocks correctly.
    """
    for block in blocks:
        text = clean_text(block)
        m_close = re.search(rf"\}}\s*;?\s*{re.escape(name)}\s*;", text)
        if not m_close:
            continue
        before = text[:m_close.start()]
        starts = [m.start() for m in re.finditer(r"typedef\s+struct\b", before)]
        if not starts:
            continue
        result = text[starts[-1]:m_close.end()]
        # Validate balanced braces
        if result.count("{") != result.count("}"):
            continue
        # Normalize trailing flexible-array members `Name[]` / `Name []`
        # to `Name[1]`. Matches EDK2's convention for UEFI spec structs
        # and sidesteps GCC's rejection of a flex array that is the
        # only named member of a struct (e.g.,
        # EFI_FILE_SYSTEM_VOLUME_LABEL). Safe to apply unconditionally:
        # in multi-member structs the spec's intent is "variable-
        # length trailing data," which `[1]` accepts identically for
        # the patterns AXL uses (sizeof-offset math, not sizeof-struct
        # allocation).
        result = re.sub(r"(\b\w+)\s*\[\s*\]\s*;", r"\1[1];", result)
        return result
    return None


def find_union(name: str, blocks: list[str]) -> str | None:
    """Find 'typedef union ... { ... } NAME;' in pre blocks."""
    for block in blocks:
        text = clean_text(block)
        m_close = re.search(rf"\}}\s*{re.escape(name)}\s*;", text)
        if not m_close:
            continue
        before = text[:m_close.start()]
        starts = [m.start() for m in re.finditer(r"typedef\s+union\b", before)]
        if not starts:
            continue
        result = text[starts[-1]:m_close.end()]
        if result.count("{") != result.count("}"):
            continue
        return result
    return None


def find_enum(name: str, blocks: list[str]) -> str | None:
    """Find 'typedef enum { ... } NAME;' in pre blocks."""
    for block in blocks:
        text = clean_text(block)
        m_close = re.search(rf"\}}\s*{re.escape(name)}\s*;", text)
        if not m_close:
            continue
        before = text[:m_close.start()]
        starts = [m.start() for m in re.finditer(r"typedef\s+enum\b", before)]
        if not starts:
            continue
        result = text[starts[-1]:m_close.end()]
        if result.count("{") != result.count("}"):
            continue
        # Fix missing commas between enum members on separate lines.
        # Only add commas between lines inside the braces, and skip
        # lines starting with // to avoid corrupting comments.
        lines = result.split("\n")
        fixed: list[str] = []
        in_body = False
        for i, line in enumerate(lines):
            if "{" in line:
                in_body = True
            if "}" in line:
                in_body = False
            stripped = line.rstrip()
            if (in_body and stripped and not stripped.startswith("//")
                    and i + 1 < len(lines)):
                next_stripped = lines[i + 1].strip()
                if (next_stripped and not next_stripped.startswith("//")
                        and not next_stripped.startswith("}")
                        and not stripped.endswith(",")
                        and not stripped.endswith("{")):
                    line = stripped + ","
            fixed.append(line)
        return "\n".join(fixed)
    return None


def find_funcptr(name: str, blocks: list[str]) -> str | None:
    """Find function pointer typedef in pre blocks.

    Handles two formats:
      1. typedef RETURN (EFIAPI *NAME) (...);   -- standard UEFI
      2. typedef RETURN NAME (...);             -- bare (runtime services)
    """
    for block in blocks:
        text = clean_text(block)
        if name not in text:
            continue

        # Format 1: (EFIAPI *NAME)
        m = re.search(rf"\(EFIAPI\s*\*\s*{re.escape(name)}\s*\)", text)
        if m:
            before = text[:m.start()]
            typedef_pos = before.rfind("typedef")
            if typedef_pos == -1:
                # Some spec blocks omit 'typedef' and/or return type
                ret_start = before.rfind("\n") + 1
                ret_text = before[ret_start:].strip()
                if not ret_text or ret_text.startswith("("):
                    text = "typedef\nEFI_STATUS\n" + text[ret_start:]
                else:
                    text = "typedef\n" + text[ret_start:]
                typedef_pos = 0
                m = re.search(rf"\(EFIAPI\s*\*\s*{re.escape(name)}\s*\)", text)
                if not m:
                    continue
            after = text[m.end():]
            close = after.find(");")
            if close == -1:
                close = after.find(")")
                if close == -1:
                    continue
                result = text[typedef_pos:m.end() + close + 1] + ";"
            else:
                result = text[typedef_pos:m.end() + close + 2]
            return result

        # Format 1b: (*NAME) without EFIAPI in the spec text. UEFI/PI
        # callbacks the firmware invokes across the ABI boundary are
        # EFIAPI in EDK2 (the spec prose just omits it); inject it so
        # x86_64 (EFIAPI == ms_abi) callers and callees agree. Omitting
        # it silently mis-passes arguments — e.g. EFI_CPU_INTERRUPT_HANDLER
        # got a garbled InterruptType, so registered exception handlers
        # were never effectively dispatched. Matches Format 2 below.
        m = re.search(rf"\(\*\s*{re.escape(name)}\s*\)", text)
        if m:
            before = text[:m.start()]
            typedef_pos = before.rfind("typedef")
            if typedef_pos == -1:
                ret_start = before.rfind("\n") + 1
                text = "typedef\n" + text[ret_start:]
                typedef_pos = 0
                m = re.search(rf"\(\*\s*{re.escape(name)}\s*\)", text)
                if not m:
                    continue
            after = text[m.end():]
            close = after.find(");")
            if close == -1:
                close = after.find(")")
                if close == -1:
                    continue
                result = text[typedef_pos:m.end() + close + 1] + ";"
            else:
                result = text[typedef_pos:m.end() + close + 2]
            return re.sub(rf"\(\s*\*\s*{re.escape(name)}\s*\)",
                          f"(EFIAPI *{name})", result, count=1)

        # Format 2: bare -- typedef\nRETURN\nNAME (
        # Require the match to be preceded by 'typedef' to avoid
        # matching comments or prose that mention the function name.
        m = re.search(rf"^\s*{re.escape(name)}\s*\(", text, re.MULTILINE)
        if m:
            before = text[:m.start()]
            typedef_pos = before.rfind("typedef")
            if typedef_pos == -1:
                continue
            # Verify nothing but whitespace/return-type between typedef and name
            between = before[typedef_pos + len("typedef"):].strip()
            if not re.match(r"^[A-Z]\w*\s*$", between):
                continue
            after = text[m.end():]
            close = after.find(");")
            if close == -1:
                close = after.find(")")
                if close == -1:
                    continue
                end_text = text[m.end():m.end() + close + 1] + ";"
            else:
                end_text = text[m.end():m.end() + close + 2]
            ret_type = between.strip()
            return f"typedef\n{ret_type}\n(EFIAPI *{name}) ({end_text}"
    return None


def find_define(name: str, blocks: list[str]) -> str | None:
    """Find '#define NAME ...' in pre blocks.

    Handles backslash continuation lines correctly.
    """
    for block in blocks:
        text = clean_text(block)
        m = re.search(rf"^#define\s+{re.escape(name)}\b.*$",
                      text, re.MULTILINE)
        if m:
            result = m.group(0)
            pos = m.end()
            while result.rstrip().endswith("\\") and pos < len(text):
                # Skip the newline character at pos
                if pos < len(text) and text[pos] == "\n":
                    pos += 1
                nl = text.find("\n", pos)
                next_line = text[pos:nl] if nl != -1 else text[pos:]
                # Stop if the continuation is just a comment
                if next_line.lstrip().startswith("//"):
                    result = result.rstrip().rstrip("\\").rstrip()
                    break
                if nl == -1:
                    result += "\n" + text[pos:]
                    break
                result += "\n" + text[pos:nl]
                pos = nl + 1
            return result
    return None


def find_typedef(name: str, blocks: list[str]) -> str | None:
    """Find 'typedef TYPE NAME;' (simple one-line typedef) in pre blocks.

    Only matches within a single line to avoid greedy cross-line matches.
    """
    for block in blocks:
        text = clean_text(block)
        # Match typedef on a single line only (no newlines in the match)
        m = re.search(
            rf"^(typedef\s+[^\n]*\b{re.escape(name)}\s*;?)$",
            text, re.MULTILINE)
        if not m:
            continue
        result = m.group(1)
        if not result.rstrip().endswith(";"):
            result = result.rstrip() + ";"
        return result
    return None


FINDERS: dict[str, Callable[[str, list[str]], str | None]] = {
    "struct": find_struct,
    "union": find_union,
    "enum": find_enum,
    "funcptr": find_funcptr,
    "define": find_define,
    "typedef": find_typedef,
}


# ===================================================================
# Post-extraction transforms
# ===================================================================

PREAMBLE_TYPES: set[str] = {
    "SHELL_FILE_HANDLE",
    "VOID", "void",
}


def add_struct_tag(code: str, name: str) -> str:
    """Ensure struct/union tag matches the forward declaration (_NAME).

    - Untagged: 'typedef struct {' → 'typedef struct _NAME {'
    - Wrong tag: 'typedef struct FOO {' → 'typedef struct _NAME {'
    - Correct tag (_NAME): no change
    """
    target_tag = f"_{name}"
    # Already has the correct tag?
    if re.search(rf"typedef\s+(struct|union)\s+{re.escape(target_tag)}\s*\{{", code):
        return code
    # Has a wrong tag — replace it
    code = re.sub(r"typedef\s+struct\s+\w+\s*\{",
                  f"typedef struct {target_tag} {{", code, count=1)
    code = re.sub(r"typedef\s+union\s+\w+\s*\{",
                  f"typedef union {target_tag} {{", code, count=1)
    # Untagged — add tag
    code = re.sub(r"typedef\s+struct\s*\{",
                  f"typedef struct {target_tag} {{", code, count=1)
    code = re.sub(r"typedef\s+union\s*\{",
                  f"typedef union {target_tag} {{", code, count=1)
    return code


def replace_unknown_types(code: str, known_types: set[str],
                          replacements: list[str] | None = None) -> str:
    """Replace unknown type references with void *.

    Handles two cases:
    1. Struct/union members: TYPE Name; -> void *Name;
    2. Function pointer params: IN TYPE *Name -> IN VOID *Name

    Only replaces types that aren't in the known_types set.
    If replacements list is provided, appends "TYPE -> void *" for each.
    """
    lines = code.split("\n")
    result: list[str] = []

    for line in lines:
        stripped = line.strip()

        if stripped.startswith("//") or stripped.startswith("#"):
            result.append(line)
            continue

        # Struct member: TYPE Name; or TYPE *Name; or TYPE Name[N];
        m = re.match(
            r"(\s*)((?:CONST\s+)?[A-Z]\w+)(\s+\*?\w+\s*(?:\[\w+\])?\s*;.*)$",
            line)
        if m:
            type_name = m.group(2).replace("CONST ", "")
            if type_name not in known_types:
                rest_m = re.match(r"\s+\*?(\w+\s*(?:\[\w+\])?\s*;.*)", m.group(3))
                if rest_m:
                    result.append(f"{m.group(1)}void  *{rest_m.group(1)}")
                    if replacements is not None:
                        replacements.append(type_name)
                    continue

        # Function param: IN/OUT TYPE *Name or IN/OUT TYPE Name
        m = re.match(
            r"(\s*(?:IN\s+OUT|IN|OUT)\s+)"
            r"((?:CONST\s+)?[A-Z]\w+)"
            r"(\s+\*{0,2}\w+.*)",
            line)
        if m:
            type_name = m.group(2).replace("CONST ", "")
            if type_name not in known_types:
                rest = m.group(3).lstrip()
                if rest.startswith("*"):
                    result.append(f"{m.group(1)}VOID {rest}")
                else:
                    result.append(f"{m.group(1)}VOID *{rest}")
                if replacements is not None:
                    replacements.append(type_name)
                continue

        result.append(line)

    return "\n".join(result)


def build_known_types(manifest: dict[str, object]) -> set[str]:
    """Build the set of all type names known from preamble + manifest."""
    known = set(PREAMBLE_TYPES)
    for hdr_name, entries in manifest.items():
        if hdr_name.startswith("_"):
            continue
        for entry in entries:
            known.add(entry["name"])
    return known


def build_forward_declarations(manifest: dict[str, object]) -> str:
    """Generate forward declarations for all struct/union types in manifest."""
    decls: list[str] = []
    seen: set[str] = set()
    for hdr_name, entries in manifest.items():
        if hdr_name.startswith("_"):
            continue
        for entry in entries:
            name = entry["name"]
            kind = entry["kind"]
            if name in seen:
                continue
            if kind == "struct":
                seen.add(name)
                decls.append(f"typedef struct _{name} {name};")
            elif kind == "union":
                seen.add(name)
                decls.append(f"typedef union _{name} {name};")
    return "\n".join(decls)


# ===================================================================
# Manifest validation
# ===================================================================

def validate_manifest(manifest: dict[str, object]) -> list[str]:
    """Validate the manifest and return a list of warnings."""
    warnings: list[str] = []
    all_names: dict[str, str] = {}

    for hdr_name, entries in manifest.items():
        if hdr_name.startswith("_"):
            continue
        if not isinstance(entries, list):
            warnings.append(f"{hdr_name}: value must be a list")
            continue
        for i, entry in enumerate(entries):
            if not isinstance(entry, dict):
                warnings.append(f"{hdr_name}[{i}]: entry must be an object")
                continue
            if "name" not in entry:
                warnings.append(f"{hdr_name}[{i}]: missing 'name' field")
                continue
            if "kind" not in entry:
                warnings.append(f"{hdr_name}[{i}]: missing 'kind' field")
                continue
            name = entry["name"]
            kind = entry["kind"]
            if kind not in VALID_KINDS:
                warnings.append(
                    f"{hdr_name}: '{name}' has unknown kind '{kind}' "
                    f"(valid: {', '.join(sorted(VALID_KINDS))})")
            if name in all_names:
                warnings.append(
                    f"{hdr_name}: '{name}' duplicates entry in "
                    f"{all_names[name]}")
            all_names[name] = hdr_name

    return warnings


# ===================================================================
# Status code extraction from Appendix D
# ===================================================================

def parse_status_codes(input_dir: Path) -> str:
    """Parse status codes from Appendix D HTML tables."""
    apx_d = input_dir / "Apx_D_Status_Codes.html"
    if not apx_d.exists():
        return "// Appendix D not found\n"

    parser = TableCellExtractor()
    parser.feed(apx_d.read_text(encoding="utf-8", errors="replace"))

    errors: list[tuple[str, int]] = []
    warnings: list[tuple[str, int]] = []

    i = 0
    while i < len(parser.cells) - 2:
        name = parser.cells[i].strip()
        val = parser.cells[i + 1].strip()
        i += 3
        if not name.startswith("EFI_"):
            continue
        try:
            v = int(val)
        except ValueError:
            continue
        if name == "EFI_SUCCESS":
            continue
        elif name.startswith("EFI_WARN_"):
            warnings.append((name, v))
        else:
            errors.append((name, v))

    lines: list[str] = []
    lines.append("#define EFI_SUCCESS  0ULL\n\n")
    lines.append("#define EFI_ERROR_BIT  0x8000000000000000ULL\n")
    lines.append("#define EFI_ERROR(x)   ((INTN)(x) < 0)\n\n")
    lines.append("// Error codes (high bit set)\n")
    for name, val in errors:
        lines.append(f"#define {name:<40s} (EFI_ERROR_BIT | {val})\n")
    lines.append("\n// Warning codes\n")
    for name, val in warnings:
        lines.append(f"#define {name:<40s} {val}\n")

    return "".join(lines)


# ===================================================================
# GUID extraction
# ===================================================================

def clean_guid_value(val: str) -> str | None:
    """Clean and validate a GUID initializer value.

    Returns the cleaned value, or None if it can't be fixed.
    Expected format: {0x..., 0x..., 0x..., {0x.., 0x.., ...}}
    """
    val = re.sub(r"\)\s*$", "}", val)
    val = re.sub(r"0x\s+([0-9A-Fa-f])", r"0x\1", val)
    val = re.sub(r"(\w)\s*(\{0x)", r"\1, \2", val)
    val = re.sub(r"(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)", r"\1,\2", val)

    # Fix misplaced inner brace: {D1,D2,D3, D4a, {D4b...}}
    m = re.match(
        r"(\{\s*0x\w+,\s*0x\w+,\s*0x\w+),?\s*(0x\w+),?\s*\{\s*(0x\w+)",
        val)
    if m:
        val = f"{m.group(1)}, {{{m.group(2)},{m.group(3)}" + val[m.end():]

    if val.count("{") != 2 or val.count("}") != 2:
        return None
    inner = val.replace("{", "").replace("}", "").replace(",", " ")
    for token in inner.split():
        if not re.match(r"^0x[0-9A-Fa-f]+$", token):
            return None
    return val


def guid_display_name(gname: str) -> str:
    """Canonical spec identifier for a GUID macro name.

    A protocol GUID's type name is the macro minus the trailing "_GUID"
    (EFI_RAM_DISK_PROTOCOL_GUID -> EFI_RAM_DISK_PROTOCOL). Every other GUID
    identifier has no shorter canonical form, so it keeps its full name
    (EFI_ACPI_TABLE_GUID stays as-is).
    """
    if gname.endswith("_PROTOCOL_GUID"):
        return gname[: -len("_GUID")]
    return gname


def write_guid_names(gnames: list[str], output_dir: Path) -> None:
    """Emit generated/guid-names.h: a (GUID, canonical-name) table.

    Backs axl_protocol_guid_name (and the `lsproto` tool) so a live handle's
    protocol GUID resolves to the exact identifier the spec/headers use --
    "EFI_RAM_DISK_PROTOCOL", not the Shell's short "RamDisk". Sorted by name.
    """
    entries = sorted(((g, guid_display_name(g)) for g in gnames),
                     key=lambda e: e[1])

    lines: list[str] = []
    lines.append("// One (GUID, canonical-name) row per generated GUID, sorted")
    lines.append("// by name. Included by exactly one .c file (static linkage).")
    lines.append("typedef struct {")
    lines.append("    const EFI_GUID *guid;   ///< pointer to the generated GUID")
    lines.append("    const char     *name;   ///< canonical spec identifier")
    lines.append("} AxlGuidNameRow;")
    lines.append("")
    lines.append("static const AxlGuidNameRow axl_guid_name_table[] = {")
    for gname, display in entries:
        lines.append(f'    {{ &{gname}, "{display}" }},')
    lines.append("};")
    lines.append("")
    lines.append(f"#define AXL_GUID_NAME_TABLE_COUNT  {len(entries)}u")

    (output_dir / "guid-names.h").write_text(
        header_wrap("guid-names.h", "\n".join(lines),
                    includes=["guids.h"],
                    description="Runtime GUID -> canonical spec-name table."))


def guid_names_from_header(guids_h: Path) -> list[str]:
    """Parse the GUID macro names out of an already-generated guids.h.

    Lets guid-names.h be regenerated from the checked-in guids.h without the
    spec HTML (which is downloaded, not committed). Same second-order-generated
    status as guids.h itself.
    """
    text = guids_h.read_text()
    # Match every generated EFI_GUID static, not only *_GUID names, so this
    # standalone path cannot silently drop a row the full run would emit
    # (guid_display_name handles any suffix). Today all are *_GUID; this keeps
    # the two paths in agreement if that ever changes.
    return re.findall(r"^static __attribute__\(\(unused\)\) EFI_GUID "
                      r"(\w+) =", text, re.MULTILINE)


def extract_guids(blocks: list[str]) -> list[tuple[str, str]]:
    """Extract all GUID #defines and convert to static const."""
    guids: list[tuple[str, str]] = []
    seen: set[str] = set()

    for block in blocks:
        text = clean_text(block)
        for m in re.finditer(
                r"#define\s+(\w+_GUID)\s*\\?\s*(\{.+?\}\s*[}\)])",
                text, re.DOTALL):
            gname = m.group(1)
            if gname in seen:
                continue
            seen.add(gname)
            gval = m.group(2).replace("\\", "").replace("\n", " ")
            gval = re.sub(r"\s+", " ", gval)
            cleaned = clean_guid_value(gval)
            if cleaned:
                guids.append((gname, cleaned))

    return guids


# ===================================================================
# Static preamble content
# ===================================================================

CALLING_H = """\
// UEFI calling convention
#if defined(__x86_64__)
#define EFIAPI  __attribute__((ms_abi))
#elif defined(__aarch64__)
#define EFIAPI  /* AARCH64 UEFI uses standard AAPCS64 */
#else
#error "Unsupported architecture -- AXL requires x86_64 or AARCH64"
#endif

#define IN
#define OUT
#define OPTIONAL
#define CONST const

#ifndef VOID
#define VOID  void
#endif
"""

TYPES_H_PREAMBLE = """\
#include "calling.h"
#include <stdint.h>
#include <stddef.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "AXL UEFI types require x86_64 or AARCH64"
#endif

// Types not in UEFI spec (Shell Spec / project-specific)
typedef void     *SHELL_FILE_HANDLE;

#ifndef TRUE
#define TRUE   1
#endif
#ifndef FALSE
#define FALSE  0
#endif
#ifndef NULL
#define NULL   ((void *)0)
#endif

#define MAX_UINTN  ((UINTN)-1)

"""

TYPES_H_EPILOGUE = """\
// EFI_TEXT_ATTR -- commented out in spec, hand-written here
#define EFI_TEXT_ATTR(fg, bg)  ((fg) | ((bg) << 4))

// EFI_GUID-typed GUID compare/equality for pure-UEFI code (no
// <axl/axl-sys.h>); the public AxlGuid versions are axl_guid_cmp() and
// axl_guid_equal() in axl-sys.h.
static inline int
axl_efi_guid_cmp(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

static inline BOOLEAN
axl_efi_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    return (BOOLEAN)(axl_efi_guid_cmp(a, b) == 0);
}
"""


# ===================================================================
# Header file wrapper
# ===================================================================

# Applications must not reach EDK2 by including a generated header directly;
# see include/uefi/axl-uefi.h for who is granted AXL_ALLOW_UEFI and why.
_UEFI_APP_GUARD = """#if !defined(AXL_ALLOW_UEFI)
#  error "<uefi/...> is not available to applications. Use the axl_* API; \
build a driver with `axl-cc --type driver` (CMake: axl_add_driver), or pass \
`--allow-uefi` (CMake: ALLOW_UEFI) to opt in deliberately."
#endif"""


def header_wrap(name: str, content: str, includes: list[str] | None = None,
                description: str = "") -> str:
    guard = f"AXL_UEFI_GEN_{name.upper().replace('-', '_').replace('.', '_')}"
    lines: list[str] = []
    lines.append(f"/* SPDX-License-Identifier: Apache-2.0 */")
    lines.append(f"/* Copyright 2026 AximCode */")
    lines.append(f"")
    lines.append(f"/** @file generated/{name}")
    lines.append(f"    Auto-generated from UEFI Specification 2.11.")
    if description:
        lines.append(f"    {description}")
    lines.append(f"    Do not edit -- regenerate with scripts/generate-uefi-headers.py")
    lines.append(f"**/\n")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}\n")
    for inc in (includes or []):
        lines.append(f'#include "{inc}"')
    if includes:
        lines.append("")
    lines.append(content)
    lines.append(f"\n#endif /* {guard} */\n")
    return "\n".join(lines)


# ===================================================================
# Source check — find UEFI types used in code but missing from manifest
# ===================================================================

# Identifiers provided by the preamble, calling.h, or C itself —
# these don't need manifest entries.
BUILTIN_UEFI_NAMES: set[str] = {
    # Preamble
    "SHELL_FILE_HANDLE", "EFI_TEXT_ATTR",
    # calling.h
    "EFIAPI", "EFI_STATUS",
    # C keywords/macros that start with EFI_ but aren't types
    "EFI_ERROR", "EFI_SUCCESS", "EFI_ERROR_BIT",
    # Common prefixes that are part of identifiers, not standalone types
    "EFI_FILE_MODE_READ", "EFI_FILE_MODE_WRITE", "EFI_FILE_MODE_CREATE",
}


def _strip_c_comments(text: str) -> str:
    """Remove C/C++ comments and string literals so we don't match prose
    that mentions UEFI types but doesn't actually use them."""
    pattern = re.compile(
        r'//[^\n]*'           # // line comments
        r'|/\*[\s\S]*?\*/'    # /* block comments */
        r'|"(?:\\.|[^"\\])*"' # "string literals"
        r"|'(?:\\.|[^'\\])*'" # 'char literals'
    )
    return pattern.sub(" ", text)


def scan_source_files(paths: list[Path]) -> dict[str, set[str]]:
    """Scan C source files for UEFI identifiers.

    Returns a dict mapping identifier name -> set of files that use it.
    Only considers EFI_*, EVT_*, TPL_* identifiers. Comments and string
    literals are stripped first so prose references don't count as use.
    """
    usage: dict[str, set[str]] = {}
    c_extensions = {".c", ".h"}

    for scan_path in paths:
        if scan_path.is_file():
            files = [scan_path]
        elif scan_path.is_dir():
            files = [f for f in scan_path.rglob("*") if f.suffix in c_extensions]
        else:
            continue

        for filepath in files:
            # Skip generated headers (they're the output, not the source)
            if "/generated/" in str(filepath):
                continue
            try:
                content = filepath.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            content = _strip_c_comments(content)
            # Find all UEFI identifiers
            for m in re.finditer(r"\b(EFI_[A-Z]\w+|EVT_[A-Z]\w+|TPL_[A-Z]\w+)\b",
                                 content):
                name = m.group(1)
                # Skip status codes (EFI_NOT_FOUND, etc.) — they're in status.h
                if name.startswith("EFI_") and name.isupper() and "_" in name[4:]:
                    # Could be a status code, a define, or a type.
                    # Status codes are handled separately, but types like
                    # EFI_BOOT_SERVICES also match. Only skip names that
                    # look like error/warning codes.
                    pass
                if name not in usage:
                    usage[name] = set()
                usage[name].add(str(filepath))

    return usage


def check_sources(scan_paths: list[Path], manifest: dict[str, object],
                  manifest_path: Path,
                  extra_header: Path | None = None) -> int:
    """Check source files for UEFI types not covered by the manifest.

    Returns 0 if all types are covered, 1 if there are missing entries.
    """
    # Build set of all names provided by the manifest + preamble
    provided: set[str] = set(PREAMBLE_TYPES) | set(BUILTIN_UEFI_NAMES)
    # Add all scalar type names
    provided.update(SCALAR_TYPE_MAP.keys())
    # Add all manifest entries and their aliases
    for hdr_name, entries in manifest.items():
        if hdr_name.startswith("_"):
            continue
        for entry in entries:
            provided.add(entry["name"])
            if "alias" in entry:
                provided.add(entry["alias"])

    # Scan extra header for typedef/struct names it provides
    if extra_header and extra_header.exists():
        extra_text = extra_header.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"}\s*(\w+)\s*;", extra_text):
            provided.add(m.group(1))
        for m in re.finditer(r"typedef\s+\w[\w\s*]*\b(\w+)\s*;", extra_text):
            provided.add(m.group(1))
        for m in re.finditer(r"#define\s+(\w+)", extra_text):
            provided.add(m.group(1))

    # Scan sources
    usage = scan_source_files(scan_paths)

    # Cross-reference
    missing: dict[str, set[str]] = {}
    for name, files in sorted(usage.items()):
        if name in provided:
            continue
        # Skip enum member names (CamelCase like EfiBootServicesData, TimerPeriodic)
        if re.match(r"Efi[A-Z]|Timer[A-Z]|Allocate[A-Z]|All[A-Z]|By[A-Z]|Tcp4|Ip4|Dns", name):
            continue
        # Skip GUID variable names (gEfi...)
        if name.startswith("gEfi"):
            continue
        # Skip status codes — these are #defines in status.h, not manifest entries
        # They follow pattern: EFI_SUCCESS, EFI_NOT_FOUND, EFI_ABORTED, EFI_WARN_*
        # Heuristic: if it's all-caps and NOT a known type pattern, it's likely a
        # status code or constant
        if name.startswith("EFI_") and name == name.upper():
            # Check if it could be a type (ends with _PROTOCOL, _DATA, _TOKEN, etc.)
            type_suffixes = ("_PROTOCOL", "_DATA", "_TOKEN", "_TABLE", "_TYPE",
                             "_MODE", "_ENTRY", "_HEADER", "_INFO", "_STATE",
                             "_POINT", "_ADDRESS", "_DESCRIPTOR", "_KEY",
                             "_DELAY", "_NOTIFY", "_RECORD")
            if not any(name.endswith(s) for s in type_suffixes):
                continue

        missing[name] = files

    if not missing:
        print(f"OK: all UEFI types in {', '.join(str(p) for p in scan_paths)} "
              f"are covered by {manifest_path}")
        return 0

    print(f"MISSING from {manifest_path}:\n")
    for name in sorted(missing):
        files = sorted(missing[name])
        file_list = ", ".join(files[:3])
        if len(files) > 3:
            file_list += f" (+{len(files) - 3} more)"
        print(f"  {name:45s} used in {file_list}")

    print(f"\n{len(missing)} UEFI types used in source but not in manifest.")
    print(f"Add them to {manifest_path} to generate their definitions.")
    return 1


# ===================================================================
# Main
# ===================================================================

def main() -> int:
    ap = argparse.ArgumentParser(description="Generate UEFI headers from spec HTML")
    ap.add_argument("--input", "-i", type=Path, nargs="+",
                    default=[Path("deps/uefi-spec"), Path("deps/pi-spec"),
                             Path("deps/acpi-spec")])
    ap.add_argument("--output", "-o", type=Path,
                    default=Path("include/uefi/generated"))
    ap.add_argument("--manifest", "-m", type=Path,
                    default=Path("scripts/uefi-manifest.json5"))
    ap.add_argument("--dump", type=Path, default=None,
                    help="Dump all extracted <pre> blocks to file and exit")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="Show details for NOT FOUND items")
    ap.add_argument("--check", type=Path, default=None, nargs="+",
                    help="Scan source dirs for UEFI types not in manifest")
    ap.add_argument("--extra-header", type=Path, default=None,
                    help="Supplemental header providing types outside the manifest")
    ap.add_argument("--guid-names-from", type=Path, default=None,
                    help="Regenerate ONLY generated/guid-names.h from an "
                         "existing guids.h (no spec HTML needed), then exit")
    args = ap.parse_args()

    # Standalone guid-names.h regeneration from the checked-in guids.h. Kept
    # spec-independent so the name table can be refreshed in any checkout; the
    # full run below regenerates it too (write_guid_names), so the two agree.
    if args.guid_names_from is not None:
        gnames = guid_names_from_header(args.guid_names_from)
        write_guid_names(gnames, args.output)
        print(f"Wrote {args.output / 'guid-names.h'} "
              f"({len(gnames)} GUIDs) from {args.guid_names_from}")
        return 0

    # Filter to input dirs that exist (PI spec may not be downloaded yet)
    input_dirs = [d for d in args.input if d.is_dir()]
    if not input_dirs:
        print(f"Error: no input directories found "
              f"({', '.join(str(d) for d in args.input)})", file=sys.stderr)
        return 1

    # Parse all <pre> blocks from all spec HTML files
    all_blocks: list[str] = []
    block_sources: list[str] = []
    for input_dir in input_dirs:
        for html_path in sorted(input_dir.glob("*.html")):
            p = PreExtractor()
            p.feed(html_path.read_text(encoding="utf-8", errors="replace"))
            for b in p.blocks:
                all_blocks.append(b)
                if args.dump:
                    block_sources.append(f"{input_dir.name}/{html_path.name}")

    print(f"Parsed {len(all_blocks)} code blocks from spec HTML")

    if args.dump:
        with open(args.dump, "w") as f:
            for i, (block, src) in enumerate(zip(all_blocks, block_sources)):
                cleaned = clean_text(block)
                f.write(f"{'='*72}\n")
                f.write(f"BLOCK {i} from {src} ({len(cleaned)} chars)\n")
                f.write(f"{'='*72}\n")
                f.write(cleaned)
                f.write("\n\n")
            table_types = parse_type_table(input_dirs[0])
            if table_types:
                f.write(f"{'='*72}\n")
                f.write(f"TABLE 2-4: Common UEFI Data Types "
                        f"({len(table_types)} types)\n")
                f.write(f"{'='*72}\n")
                for tname, tdef in sorted(table_types.items()):
                    f.write(f"{tdef}\n")
                f.write("\n")
        print(f"Dumped {len(all_blocks)} blocks + "
              f"{len(table_types)} table types to {args.dump}")
        return 0

    manifest = load_json5(args.manifest)

    if args.check:
        return check_sources(args.check, manifest, args.manifest,
                             args.extra_header)

    # Validate manifest
    manifest_warnings = validate_manifest(manifest)
    for w in manifest_warnings:
        print(f"  MANIFEST WARNING: {w}")
    if manifest_warnings:
        print()

    output_dir = args.output
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write static headers
    (output_dir / "calling.h").write_text(
        header_wrap("calling.h", CALLING_H,
                    description="UEFI calling convention macros."))

    (output_dir / "types.h").write_text(
        header_wrap("types.h", TYPES_H_PREAMBLE,
                    description="Base UEFI types, constants, and enums."))

    status_content = parse_status_codes(input_dirs[0])
    (output_dir / "status.h").write_text(
        header_wrap("status.h", status_content,
                    includes=["types.h"],
                    description="EFI status codes from Appendix D."))

    # Parse Table 2-4 for scalar/opaque type definitions
    table_types = parse_type_table(input_dirs[0])

    # Build known types and forward declarations from manifest
    known_types = build_known_types(manifest)

    # Also scan the extra header for types it defines (hand-written structs
    # like EFI_FILE_PROTOCOL, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL).
    # Without this, the generator replaces references to these types with
    # void * because it doesn't know they exist.
    if args.extra_header and args.extra_header.exists():
        extra_text = args.extra_header.read_text(
            encoding="utf-8", errors="replace")
        for m in re.finditer(r"}\s*(\w+)\s*;", extra_text):
            known_types.add(m.group(1))
        for m in re.finditer(
                r"typedef\s+(?:struct|union|enum)\s+\w+\s*\{[^}]*}\s*(\w+)\s*;",
                extra_text, re.DOTALL):
            known_types.add(m.group(1))

    fwd_decls = build_forward_declarations(manifest)

    # Add forward declarations for extra-header struct types so generated
    # funcptrs can reference them before axl-uefi-extra.h is included.
    # Only for tagged structs (typedef struct _TAG { ... } NAME;) where
    # we can emit a matching forward declaration.
    if args.extra_header and args.extra_header.exists():
        extra_text = args.extra_header.read_text(
            encoding="utf-8", errors="replace")
        for m in re.finditer(
                r"typedef\s+struct\s+(_\w+)\s*\{[^}]*}\s*(\w+)\s*;",
                extra_text, re.DOTALL):
            tag = m.group(1)
            name = m.group(2)
            if name not in known_types:
                continue
            decl = f"typedef struct {tag} {name};"
            if decl not in fwd_decls:
                fwd_decls += "\n" + decl

    # Process manifest
    total_found = 0
    total_missing = 0
    total_voided = 0
    header_names: list[str] = []

    for hdr_name, entries in manifest.items():
        if hdr_name.startswith("_"):
            continue

        defs: list[str] = []
        for entry in entries:
            name = entry["name"]
            kind = entry["kind"]
            search_name = entry.get("alias", name)

            if kind == "table":
                result = find_table_type(search_name, table_types)
            else:
                finder = FINDERS.get(kind)
                if not finder:
                    continue
                result = finder(search_name, all_blocks)

            if result and search_name != name:
                result += f"\ntypedef {search_name} {name};"

            if result:
                if kind in ("struct", "union"):
                    result = add_struct_tag(result, name)
                if kind in ("struct", "union", "funcptr"):
                    original = result
                    void_list: list[str] = []
                    result = replace_unknown_types(
                        result, known_types, void_list)
                    if void_list:
                        total_voided += len(void_list)
                        if args.verbose:
                            for vt in void_list:
                                print(f"    {name}: {vt} -> void *")
                defs.append(result)
                total_found += 1
            else:
                msg = f"  NOT FOUND: {kind} {name}"
                if search_name != name:
                    msg += f" (searched as {search_name})"
                print(msg)
                if args.verbose:
                    # Show blocks that contain the search name
                    for i, block in enumerate(all_blocks):
                        if search_name in block:
                            preview = clean_text(block)[:120].replace("\n", "\\n")
                            print(f"    block {i}: {preview}")
                total_missing += 1

        if defs:
            content = "\n\n".join(defs) + "\n"
            if hdr_name == "types.h":
                (output_dir / hdr_name).write_text(
                    header_wrap(hdr_name,
                                TYPES_H_PREAMBLE + "\n" + content + "\n"
                                + TYPES_H_EPILOGUE,
                                description="Base UEFI types, constants, and enums."))
            else:
                (output_dir / hdr_name).write_text(
                    header_wrap(hdr_name, content,
                                includes=["types.h", "status.h"]))
            header_names.append(hdr_name)
            print(f"  {hdr_name:<25s} {len(defs)} definitions")

    # Extract and write GUIDs
    guids = extract_guids(all_blocks)
    guid_lines: list[str] = []
    for gname, gval in guids:
        guid_lines.append(f"static __attribute__((unused)) EFI_GUID {gname} =")
        guid_lines.append(f"    {gval};\n")

    guid_lines.append("\n// EDK2-style GUID aliases")
    for gname, _ in guids:
        if gname.startswith("EFI_"):
            parts = gname[4:].split("_")
            edk2 = "gEfi" + "".join(p.capitalize() for p in parts)
            if edk2 != gname:
                guid_lines.append(f"#define {edk2}  {gname}")

    (output_dir / "guids.h").write_text(
        header_wrap("guids.h", "\n".join(guid_lines),
                    includes=["types.h"],
                    description="All UEFI protocol GUIDs."))

    # Runtime GUID -> canonical spec-name table (see write_guid_names).
    write_guid_names([g for g, _ in guids], output_dir)

    # Write all.h umbrella with forward declarations
    all_lines: list[str] = []
    all_lines.append('#include "calling.h"')
    all_lines.append('#include "types.h"')
    all_lines.append('#include "status.h"')
    all_lines.append("")
    all_lines.append("// Forward declarations for all manifest struct/union types")
    all_lines.append(fwd_decls)
    all_lines.append("")
    for hdr in header_names:
        if hdr in ("types.h", "calling.h", "status.h"):
            continue
        all_lines.append(f'#include "{hdr}"')
    all_lines.append('#include "guids.h"')
    all_lines.append("")

    # The umbrella carries the same application guard as the hand-written
    # <uefi/axl-uefi.h>. Emitted HERE rather than hand-edited into all.h,
    # because all.h is regenerated from spec HTML and a manual edit would be
    # silently lost on the next run -- taking the guard with it.
    all_lines = [_UEFI_APP_GUARD, ""] + all_lines

    (output_dir / "all.h").write_text(
        header_wrap("all.h", "\n".join(all_lines),
                    description="Umbrella -- includes all generated UEFI headers."))

    hdr_count = 3 + len(header_names) + 2
    print(f"\nGenerated {hdr_count} headers ({total_found} found, "
          f"{total_missing} missing, {total_voided} void-replaced, "
          f"{len(guids)} GUIDs)")

    return 1 if total_missing > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
