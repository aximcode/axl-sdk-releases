#!/usr/bin/env python3
"""Generate UEFI headers from spec HTML.

Extracts all C code blocks from the spec HTML, cleans RST artifacts,
deduplicates by name, classifies blocks, and emits into chapter-based
header files with correct dependency ordering.

Usage:
    python3 scripts/generate-uefi-headers.py [--input DIR] [--output DIR] [--dump]
"""

from __future__ import annotations

import argparse
import re
import sys
from html.parser import HTMLParser
from pathlib import Path


# ===================================================================
# Chapter → output header mapping
# ===================================================================

CHAPTER_MAP: dict[str, str] = {
    "04_EFI_System_Table.html": "tables.h",
    "07_Services_Boot_Services.html": "tables.h",
    "08_Services_Runtime_Services.html": "tables.h",
    "09_Protocols_EFI_Loaded_Image.html": "image.h",
    "10_Protocols_Device_Path_Protocol.html": "device-path.h",
    "11_Protocols_UEFI_Driver_Model.html": "driver-model.h",
    "12_Protocols_Console_Support.html": "console.h",
    "13_Protocols_Media_Access.html": "media.h",
    "14_Protocols_PCI_Bus_Support.html": "pci.h",
    "15_Protocols_SCSI_Driver_Models_and_Bus_Support.html": "scsi.h",
    "16_Protocols_iSCSI_Boot.html": "iscsi.h",
    "17_Protocols_USB_Support.html": "usb.h",
    "18_Protocols_Debugger_Support.html": "debug.h",
    "20_Protocols_ACPI_Protocols.html": "acpi.h",
    "21_Protocols_String_Services.html": "misc.h",
    "23_Firmware_Update_and_Reporting.html": "misc.h",
    "24_Network_Protocols_SNP_PXE_BIS.html": "network.h",
    "25_Network_Protocols_Managed_Network.html": "network.h",
    "26_Network_Protocols_Bluetooth.html": "network.h",
    "27_Network_Protocols_VLAN_and_EAP.html": "network.h",
    "28_Network_Protocols_TCP_IP_and_Configuration.html": "network.h",
    "29_Network_Protocols_ARP_and_DHCP.html": "network.h",
    "30_Network_Protocols_UDP_and_MTFTP.html": "network.h",
    "31_EFI_Redfish_Service_Support.html": "network.h",
    "32_Secure_Boot_and_Driver_Signing.html": "secure.h",
    "33_Human_Interface_Infrastructure.html": "hii.h",
    "34_HII_Protocols.html": "hii.h",
    "35_HII_Configuration_Processing_and_Browser_Protocol.html": "hii.h",
    "36_User_Identification.html": "secure.h",
    "37_Secure_Technologies.html": "secure.h",
    "38_Confidential_Computing.html": "secure.h",
    "39_Micellaneous_Protocols.html": "misc.h",
}

# Chapters to skip entirely (no useful C definitions)
SKIP_CHAPTERS: set[str] = {
    "index.html",
    "01_Introduction.html",
    "02_Overview.html",
    "03_Boot_Manager.html",
    "05_GUID_Partition_Table_Format.html",
    "06_Block_Transition_Table_Layout.html",
    "19_Protocols_Compression_Algorithm_Specification.html",
    "22_EFI_Byte_Code_Virtual_Machine.html",
}


# ===================================================================
# HTML parser — extract <pre> block text
# ===================================================================

class PreExtractor(HTMLParser):
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


# ===================================================================
# HTML table parser — for status codes in Appendix D
# ===================================================================

class TableCellExtractor(HTMLParser):
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
# Code cleaning
# ===================================================================

def clean_code(text: str) -> str:
    """Clean RST/Sphinx artifacts from extracted code."""
    # Replace non-ASCII characters
    text = text.replace("\u2026", "...")  # horizontal ellipsis
    text = text.replace("\u2019", "'")   # right single quote
    text = text.replace("\u201c", '"')   # left double quote
    text = text.replace("\u201d", '"')   # right double quote

    # RST escapes
    text = text.replace("\\_", "_")
    text = text.replace("\\*", "*")
    text = text.replace("\\|", "|")

    # Remove leading labels
    text = re.sub(r"^(Prototype|Related Definitions|Summary|Description)\s*\n",
                  "", text)

    # Remove RST bold markers that leaked through as C code
    text = re.sub(r"^\*\*\w[\w\s]*\*\*\s*$", "", text, flags=re.MULTILINE)

    # Remove lines that are just dots (ellipsis filler from spec)
    text = re.sub(r"^\.{2,}\s*$", "", text, flags=re.MULTILINE)

    # Fix doubled backslashes in #define continuations
    text = text.replace("\\\\\n", "\\\n")

    # Convert /*** comment blocks to // comments or remove
    text = re.sub(r"/\*{2,}.*?\*{2,}/", "", text, flags=re.DOTALL)
    text = re.sub(r"^/\*{2,}[^/]*$", "", text, flags=re.MULTILINE)
    text = re.sub(r"^/\*[^*].*$",
                  lambda m: "// " + m.group(0)[2:].strip()
                  if not m.group(0).rstrip().endswith("*/") else m.group(0),
                  text, flags=re.MULTILINE)

    # Fix double-paren in function pointers
    text = re.sub(r"\(EFIAPI \*(\w+)\) \(\(", r"(EFIAPI *\1) (", text)

    # Fix misplaced EFIAPI star: (* EFIAPI NAME) → (EFIAPI *NAME)
    text = re.sub(r"\(\*\s+EFIAPI\s+(\w+)\)", r"(EFIAPI *\1)", text)

    # Fix missing ( before EFIAPI in function pointer typedef
    # Pattern: line starts with "EFIAPI *NAME) ("
    text = re.sub(r"^(EFIAPI\s+\*\w+\)\s*\()", r"(\1", text, flags=re.MULTILINE)

    # Fix missing ( after function pointer name
    text = re.sub(r"(\(EFIAPI \*\w+\))\s*\n(\s+IN )", r"\1 (\n\2", text)

    # Fix OPTIONAL,  → OPTIONAL (remove errant comma before OPTIONAL)
    text = re.sub(r",\s*OPTIONAL\s*,", " OPTIONAL,", text)
    text = re.sub(r",\s*OPTIONAL\s*\n", " OPTIONAL\n", text)

    # Fix OPTIONAL missing comma before next parameter
    text = re.sub(r"(OPTIONAL)\s*\n(\s+(?:IN |OUT |IN OUT ))", r"\1,\n\2", text)

    # Fix trailing comma before ); in function pointer params
    text = re.sub(r",\s*\n(\s*\);)", r"\n\1", text)

    # Fix space in type name: EFI_SIMPLE_FILE_SYSTEM PROTOCOL → _PROTOCOL
    text = re.sub(r"(EFI_\w+)\s+(PROTOCOL)", r"\1_\2", text)

    # Fix split _GUID: PROTOCOL _GUID → PROTOCOL_GUID
    text = re.sub(r"(\w)\s+(_GUID\b)", r"\1\2", text)

    # Fix struct_NAME → struct _NAME (missing space)
    text = re.sub(r"\bstruct_(\w)", r"struct _\1", text)

    # Fix double underscore: EFI__HANDLE → EFI_HANDLE
    text = re.sub(r"\bEFI__(\w)", r"EFI_\1", text)

    # Fix case error: EFI_System_Table → EFI_SYSTEM_TABLE
    text = text.replace("EFI_System_Table", "EFI_SYSTEM_TABLE")

    # Fix RST bold around identifiers: *Name* → Name (but not *Name for pointers)
    text = re.sub(r"(?<=[\s,])\*(\w+)\*(?=\s)", r"\1", text)

    # Fix bare function typedef: "typedef\nRETURN\nFuncName (" → "(EFIAPI *EFI_FUNC_NAME) ("
    # Runtime service typedefs in the spec use bare function names
    def _fix_bare_func_typedef(m: re.Match[str]) -> str:
        ret_type = m.group(1)
        func_name = m.group(2)
        # Convert CamelCase to SCREAMING_SNAKE_CASE with EFI_ prefix
        screaming = re.sub(r"([a-z])([A-Z])", r"\1_\2", func_name)
        screaming = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", screaming)
        efi_name = "EFI_" + screaming.upper()
        return f"{ret_type}\n(EFIAPI *{efi_name}) ("

    text = re.sub(
        r"^((?:VOID|EFI_STATUS|UINTN|BOOLEAN|CHAR16\s*\*|EFI_DEVICE_PATH_PROTOCOL\s*\*))\s*\n"
        r"([A-Z][a-zA-Z0-9]+)\s*\(",
        _fix_bare_func_typedef,
        text, flags=re.MULTILINE)

    # Fix bare #define without #
    text = re.sub(r"^define\s+(EFI_\w)", r"#define \1", text, flags=re.MULTILINE)

    # Remove trailing backslash from struct member lines (not #define continuations)
    text = re.sub(r"(\w)\\\s+(\w)", r"\1  \2", text)

    # Fix broken define: #define NAME(EXPR) VALUE → #define NAME VALUE
    # This happens when spec inlines the expression as a parenthesized argument
    text = re.sub(r"^(#define\s+\w+)\([^)]*\|[^)]*\)\s+(0x\w+)$",
                  r"\1  \2", text, flags=re.MULTILINE)

    # Normalize blank lines
    text = re.sub(r"\n[ \t]+\n", "\n\n", text)

    text = text.strip()

    # Fix } NAME without trailing ; (struct/union/enum closing)
    # Use [ \t] to avoid crossing newlines
    text = re.sub(r"^(\}[ \t]*\w+)[ \t]*$", lambda m: m.group(1) + ";"
                  if not m.group(1).rstrip().endswith(";") else m.group(0),
                  text, flags=re.MULTILINE)

    # Fix single-line typedef missing ;
    # e.g. "typedef VOID *EFI_EVENT" without semicolon
    # Must be a complete typedef on one line (not struct/enum/union opener)
    text = re.sub(r"^(typedef[ \t]+(?!struct\b|enum\b|union\b)\w[\w \t*]+\w)[ \t]*$",
                  lambda m: m.group(1) + ";"
                  if not m.group(1).endswith(";") else m.group(0),
                  text, flags=re.MULTILINE)

    # Ensure function pointer typedefs end with ;
    if text.startswith("typedef") and "EFIAPI" in text:
        if text.endswith(")") and not text.endswith(");"):
            text += ";"

    # Strip trailing prose after the last complete definition
    # (e.g., text after "} NAME;" that's English prose, not C)
    m = re.search(r"(}\s*\w*\s*;)\s*\n", text)
    if m:
        after = text[m.end():]
        # Check if everything after the last }; is non-C prose
        after_stripped = after.strip()
        if after_stripped and not re.match(
                r"(typedef|#define|struct|enum|union|//|/\*|\}|"
                r"[A-Z_][A-Z_0-9]+\s)", after_stripped):
            text = text[:m.end()].rstrip()

    return text


def is_c_definition(block: str) -> bool:
    """Check if a code block contains a C type/macro definition."""
    if not re.match(r"\s*(typedef\s|#define\s|struct\s|enum\s|//)", block):
        return False
    # Reject blocks that are mostly prose (no C syntax tokens)
    c_tokens = sum(1 for ch in block if ch in "{}();#")
    if c_tokens == 0 and "typedef" not in block and "#define" not in block:
        return False
    return True


def is_valid_block(block: str) -> bool:
    """Check if a cleaned block is structurally valid C."""
    # Reject orphaned closing braces without matching open
    if "{" not in block and re.search(r"^\s*\}", block, re.MULTILINE):
        return False
    # Reject blocks that are just a closing brace + name
    if re.match(r"^\s*\}\s*\w+\s*;?\s*$", block):
        return False
    # Reject blocks where #define is followed by prose description
    # (e.g. "EFI_ABSP_SupportsAltActive\n  This bit is set if...")
    lines = block.split("\n")
    if len(lines) > 1:
        has_prose = False
        for line in lines:
            stripped = line.strip()
            if not stripped or stripped.startswith("//") or stripped.startswith("#"):
                continue
            if stripped.startswith("typedef") or stripped.startswith("{"):
                continue
            if stripped.startswith(("IN ", "OUT ", "IN OUT ")):
                continue
            # Check for English prose (multiple words not looking like C)
            words = stripped.split()
            if len(words) >= 3 and any(w.lower() in (
                "is", "the", "this", "bit", "set", "if", "indicates",
                "that", "when", "are", "has", "can", "will", "may",
                "should", "must", "not", "and", "or", "for", "with",
                "from", "value", "field", "active", "supported",
            ) for w in words):
                has_prose = True
                break
        if has_prose and block.count("{") == 0 and block.count("typedef") == 0:
            return False
    return True


def split_block(block: str) -> list[str]:
    """Split a block containing multiple definitions into separate blocks.

    Many spec <pre> blocks contain a #define followed by a typedef struct,
    or multiple typedef structs. Split at boundaries where a new definition
    starts (typedef, #define at start of line after prior content).
    """
    lines = block.split("\n")
    parts: list[str] = []
    current: list[str] = []
    in_braces = 0

    for line in lines:
        stripped = line.strip()

        # Track brace nesting
        in_braces += stripped.count("{") - stripped.count("}")

        # Check if this line starts a new definition
        is_new_def = (
            in_braces == 0 and current and
            (stripped.startswith("typedef ") or
             (stripped.startswith("#define ") and
              any(l.strip().endswith(";") for l in current)))
        )

        if is_new_def:
            part = "\n".join(current).strip()
            if part:
                parts.append(part)
            current = [line]
            in_braces = stripped.count("{") - stripped.count("}")
        else:
            current.append(line)

    if current:
        part = "\n".join(current).strip()
        if part:
            parts.append(part)

    return parts if len(parts) > 1 else [block]


def extract_name(block: str) -> str | None:
    """Extract the primary name from a C definition for dedup."""
    # typedef struct/enum/union ... } NAME;
    m = re.search(r"}\s*(\w+)\s*;", block)
    if m:
        return m.group(1)

    # typedef ... (EFIAPI *NAME)(
    m = re.search(r"\(EFIAPI\s*\*\s*(\w+)\)", block)
    if m:
        return m.group(1)

    # #define NAME
    m = re.match(r"#define\s+(\w+)", block)
    if m:
        return m.group(1)

    # typedef TYPE NAME;  (simple typedef)
    m = re.match(r"typedef\s+\w+[\s*]+(\w+)\s*;", block)
    if m:
        return m.group(1)

    return None


# ===================================================================
# Block classification for dependency ordering
# ===================================================================

def classify_block(block: str) -> int:
    """Classify a block for emission ordering.

    Returns priority (lower = emitted first):
      0 — #define macros and comment-only blocks
      1 — simple typedefs (typedef X Y;) and enums
      2 — function pointer typedefs (typedef ... (EFIAPI *NAME)(...))
      3 — struct/union typedefs (protocol structs that use func ptr members)
    """
    # Look past leading comments to find the actual content
    content = block
    while content.lstrip().startswith("//"):
        # Skip comment lines
        idx = content.find("\n")
        if idx == -1:
            break
        content = content[idx + 1:]
    stripped = content.lstrip()

    # Check for typedef in the block (may follow comments)
    if "typedef" in block:
        # Function pointer typedef
        if "(EFIAPI" in block:
            return 2
        # Struct/union typedef (multi-line with braces)
        if re.search(r"typedef\s+struct\b", block) and "{" in block:
            return 3
        if re.search(r"typedef\s+union\b", block) and "{" in block:
            return 3
        # Enum typedef
        if re.search(r"typedef\s+enum\s", block):
            return 1
        # Simple typedef (single line, no braces)
        if "{" not in block:
            return 1

    if stripped.startswith("#define") or not stripped:
        return 0

    # Comment-only blocks
    if all(line.strip().startswith("//") or not line.strip()
           for line in block.split("\n")):
        return 0

    return 3


def extract_member_types(block: str) -> set[str]:
    """Extract type names referenced as struct/union members."""
    types: set[str] = set()
    for line in block.split("\n"):
        stripped = line.strip()
        # Match member declarations: TYPE  Name;
        m = re.match(r"((?:CONST\s+)?[A-Z]\w+)\s+\*?\w+", stripped)
        if m:
            types.add(m.group(1).replace("CONST ", ""))
    return types


def topo_sort_structs(struct_blocks: list[tuple[int, str]]) -> list[str]:
    """Topologically sort struct blocks by their member type dependencies."""
    # Build name → block index mapping
    name_to_idx: dict[str, int] = {}
    idx_to_name: dict[int, str] = {}
    for i, (orig_idx, block) in enumerate(struct_blocks):
        name = extract_name(block)
        if name:
            name_to_idx[name] = i
            idx_to_name[i] = name

    # Build dependency graph
    deps: dict[int, set[int]] = {i: set() for i in range(len(struct_blocks))}
    for i, (orig_idx, block) in enumerate(struct_blocks):
        member_types = extract_member_types(block)
        for t in member_types:
            if t in name_to_idx and name_to_idx[t] != i:
                deps[i].add(name_to_idx[t])

    # Count how many other nodes depend on each node (out-degree)
    depended_by: dict[int, int] = {i: 0 for i in range(len(struct_blocks))}
    for i, dep_set in deps.items():
        for d in dep_set:
            depended_by[d] += 1

    # Kahn's algorithm for topological sort
    in_degree: dict[int, int] = {i: len(deps[i])
                                  for i in range(len(struct_blocks))}

    def sort_key(i: int) -> tuple[int, int]:
        # Prefer nodes depended-upon by others (higher = first),
        # then by document order for ties
        return (-depended_by[i], struct_blocks[i][0])

    queue = sorted([i for i in range(len(struct_blocks))
                    if in_degree[i] == 0], key=sort_key)

    result: list[str] = []
    visited: set[int] = set()
    while queue:
        node = queue.pop(0)
        if node in visited:
            continue
        visited.add(node)
        result.append(struct_blocks[node][1])
        # Find nodes that depend on this one and update
        new_ready: list[int] = []
        for i in range(len(struct_blocks)):
            if node in deps[i]:
                deps[i].discard(node)
                if not deps[i] and i not in visited:
                    new_ready.append(i)
        if new_ready:
            new_ready.sort(key=sort_key)
            queue.extend(new_ready)

    # Append any remaining (cyclic) blocks in original order
    for i in range(len(struct_blocks)):
        if i not in visited:
            result.append(struct_blocks[i][1])

    return result


def reorder_blocks(blocks: list[str]) -> list[str]:
    """Reorder blocks so dependencies are satisfied.

    Protocol structs reference function pointer typedefs (as members),
    and function pointer typedefs reference protocol structs (as *This
    parameter type).  Break the cycle with forward declarations:

    Emit order:
      1. Forward declarations for all tagged struct types
      2. #define macros
      3. Simple typedefs and enums
      4. Function pointer typedefs
      5. Struct/union typedefs (topologically sorted by member deps)

    Within each non-struct category, original document order is preserved.
    """
    # Collect forward declarations for ALL struct/union typedef types
    fwd_decls: list[str] = []
    seen_fwd: set[str] = set()
    for block in blocks:
        # Tagged structs: typedef struct TAG { ... } NAME;
        m = re.search(r"typedef\s+struct\s+(\w+)\s*\{", block)
        if m:
            tag = m.group(1)
            m2 = re.search(r"}\s*(\w+)\s*;", block)
            if m2:
                name = m2.group(1)
                if name not in seen_fwd:
                    seen_fwd.add(name)
                    fwd_decls.append(
                        f"typedef struct {tag} {name};")
            continue
        # Untagged structs: typedef struct { ... } NAME;
        m = re.search(r"typedef\s+struct\s*\{", block)
        if m:
            m2 = re.search(r"}\s*(\w+)\s*;", block)
            if m2:
                name = m2.group(1)
                if name not in seen_fwd:
                    seen_fwd.add(name)
                    fwd_decls.append(
                        f"typedef struct _{name} {name};")
            continue
        # Unions: typedef union { ... } NAME;
        m = re.search(r"typedef\s+union\s*\{", block)
        if m:
            m2 = re.search(r"}\s*(\w+)\s*;", block)
            if m2:
                name = m2.group(1)
                if name not in seen_fwd:
                    seen_fwd.add(name)
                    fwd_decls.append(
                        f"typedef union _{name} {name};")

    # Add tags to untagged structs/unions that got forward declarations
    tagged_blocks: list[str] = []
    for block in blocks:
        if re.search(r"typedef\s+struct\s*\{", block):
            m2 = re.search(r"}\s*(\w+)\s*;", block)
            if m2 and m2.group(1) in seen_fwd:
                name = m2.group(1)
                block = re.sub(r"typedef\s+struct\s*\{",
                               f"typedef struct _{name} {{", block, count=1)
        elif re.search(r"typedef\s+union\s*\{", block):
            m2 = re.search(r"}\s*(\w+)\s*;", block)
            if m2 and m2.group(1) in seen_fwd:
                name = m2.group(1)
                block = re.sub(r"typedef\s+union\s*\{",
                               f"typedef union _{name} {{", block, count=1)
        tagged_blocks.append(block)
    blocks = tagged_blocks

    # Classify and separate
    non_structs: list[tuple[int, int, str]] = []  # (priority, orig_idx, block)
    struct_blocks: list[tuple[int, str]] = []     # (orig_idx, block)

    for i, block in enumerate(blocks):
        prio = classify_block(block)
        if prio == 3:
            struct_blocks.append((i, block))
        else:
            non_structs.append((prio, i, block))

    non_structs.sort(key=lambda x: (x[0], x[1]))
    sorted_structs = topo_sort_structs(struct_blocks)

    result = fwd_decls
    result += [b for _, _, b in non_structs]
    result += sorted_structs
    return result


# ===================================================================
# Status code extraction from Appendix D
# ===================================================================

def parse_status_codes(input_dir: Path) -> str:
    """Parse status codes from Appendix D HTML tables."""
    apx_d = input_dir / "Apx_D_Status_Codes.html"
    if not apx_d.exists():
        return "// Appendix D not found — status codes not generated\n"

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
# Static preambles (types not in <pre> blocks)
# ===================================================================

CALLING_H_CONTENT = """\
// UEFI calling convention
#if defined(__x86_64__)
#define EFIAPI  __attribute__((ms_abi))
#elif defined(__aarch64__)
#define EFIAPI  /* AARCH64 UEFI uses standard AAPCS64 */
#else
#error "Unsupported architecture -- AXL requires x86_64 or AARCH64"
#endif

// Parameter decoration (no-op, matches UEFI spec convention)
#define IN
#define OUT
#define OPTIONAL
#define CONST const

#ifndef VOID
#define VOID  void
#endif
"""

TYPES_H_CONTENT = """\
#include "calling.h"
#include <stdint.h>
#include <stddef.h>

// ===================================================================
// Architecture check
// ===================================================================

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "AXL UEFI types require x86_64 or AARCH64"
#endif

// ===================================================================
// Scalar types (UEFI Spec 2.11, Table 2-4)
// ===================================================================

typedef uint8_t   BOOLEAN;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef char      CHAR8;
typedef uint16_t  CHAR16;

typedef uint64_t  UINTN;
typedef int64_t   INTN;

// ===================================================================
// Handle and opaque types
// ===================================================================

typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINTN     EFI_STATUS;
typedef UINTN     EFI_TPL;
typedef void     *SHELL_FILE_HANDLE;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;
typedef UINT64    EFI_LBA;

// ===================================================================
// Constants
// ===================================================================

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

// ===================================================================
// Event type flags
// ===================================================================

#define EVT_TIMER          0x80000000
#define EVT_RUNTIME        0x40000000
#define EVT_NOTIFY_WAIT    0x00000100
#define EVT_NOTIFY_SIGNAL  0x00000200

// ===================================================================
// TPL levels
// ===================================================================

#define TPL_APPLICATION  4
#define TPL_CALLBACK     8
#define TPL_NOTIFY       16

// ===================================================================
// Composite types
// ===================================================================

typedef struct {
    UINT32  Data1;
    UINT16  Data2;
    UINT16  Data3;
    UINT8   Data4[8];
} EFI_GUID;

typedef struct {
    UINT16  Year;
    UINT8   Month;
    UINT8   Day;
    UINT8   Hour;
    UINT8   Minute;
    UINT8   Second;
    UINT8   Pad1;
    UINT32  Nanosecond;
    INT16   TimeZone;
    UINT8   Daylight;
    UINT8   Pad2;
} EFI_TIME;

typedef struct {
    UINT32   Resolution;
    UINT32   Accuracy;
    BOOLEAN  SetsToZero;
} EFI_TIME_CAPABILITIES;

typedef struct {
    UINT16  ScanCode;
    CHAR16  UnicodeChar;
} EFI_INPUT_KEY;

typedef struct {
    UINT8  Addr[4];
} EFI_IPv4_ADDRESS;

typedef struct {
    UINT8  Addr[32];
} EFI_MAC_ADDRESS;

typedef struct {
    UINT8  Addr[16];
} EFI_IPv6_ADDRESS;

// ===================================================================
// Enums needed by Boot/Runtime Services
// ===================================================================

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode, EfiLoaderData,
    EfiBootServicesCode, EfiBootServicesData,
    EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS,
    EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    TimerCancel, TimerPeriodic, TimerRelative
} EFI_TIMER_DELAY;

typedef enum {
    AllHandles, ByRegisterNotify, ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef enum {
    AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EFI_NATIVE_INTERFACE
} EFI_INTERFACE_TYPE;

typedef enum {
    EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific
} EFI_RESET_TYPE;

// ===================================================================
// Structures needed by Boot/Runtime Services
// ===================================================================

typedef struct {
    UINT32                Type;
    EFI_PHYSICAL_ADDRESS  PhysicalStart;
    EFI_VIRTUAL_ADDRESS   VirtualStart;
    UINT64                NumberOfPages;
    UINT64                Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_HANDLE  AgentHandle;
    EFI_HANDLE  ControllerHandle;
    UINT32      Attributes;
    UINT32      OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef struct {
    EFI_GUID  CapsuleGuid;
    UINT32    HeaderSize;
    UINT32    Flags;
    UINT32    CapsuleImageSize;
} EFI_CAPSULE_HEADER;

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(
    IN EFI_EVENT  Event,
    IN VOID      *Context
    );

// Forward declarations
typedef struct _EFI_DEVICE_PATH_PROTOCOL {
    UINT8  Type;
    UINT8  SubType;
    UINT8  Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

// Opaque handles used by various protocols
typedef void  *EFI_HII_HANDLE;
typedef UINT16 EFI_STRING_ID;
typedef UINT16 EFI_IMAGE_ID;
typedef UINT16 EFI_FONT_HANDLE;
typedef UINT16 EFI_QUESTION_ID;
typedef CHAR16 *EFI_STRING;

// ===================================================================
// Console text attributes
// ===================================================================

#define EFI_BLACK         0x00
#define EFI_BLUE          0x01
#define EFI_GREEN         0x02
#define EFI_CYAN          0x03
#define EFI_RED           0x04
#define EFI_MAGENTA       0x05
#define EFI_BROWN         0x06
#define EFI_LIGHTGRAY     0x07
#define EFI_DARKGRAY      0x08
#define EFI_LIGHTBLUE     0x09
#define EFI_LIGHTGREEN    0x0A
#define EFI_LIGHTCYAN     0x0B
#define EFI_LIGHTRED      0x0C
#define EFI_LIGHTMAGENTA  0x0D
#define EFI_YELLOW        0x0E
#define EFI_WHITE         0x0F

#define EFI_TEXT_ATTR(fg, bg)  ((fg) | ((bg) << 4))

// ===================================================================
// File constants
// ===================================================================

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL
#define EFI_FILE_DIRECTORY    0x0000000000000010ULL
#define EFI_FILE_READ_ONLY    0x0000000000000001ULL

// ===================================================================
// Table header (UEFI Spec 2.11, Table 4-2)
// ===================================================================

typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

// ===================================================================
// Console protocols (needed before EFI_SYSTEM_TABLE)
// ===================================================================

typedef struct {
    INT32    MaxMode;
    INT32    Mode;
    INT32    Attribute;
    INT32    CursorColumn;
    INT32    CursorRow;
    BOOLEAN  CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
    IN BOOLEAN                           ExtendedVerification
    );

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
    IN CHAR16                           *String
    );

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
    IN UINTN                            Attribute
    );

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET          Reset;
    EFI_TEXT_STRING         OutputString;
    void                   *TestString;
    void                   *QueryMode;
    void                   *SetMode;
    EFI_TEXT_SET_ATTRIBUTE  SetAttribute;
    void                   *ClearScreen;
    void                   *SetCursorPosition;
    void                   *EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL  EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
    IN BOOLEAN                          ExtendedVerification
    );

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
    OUT EFI_INPUT_KEY                   *Key
    );

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET     Reset;
    EFI_INPUT_READ_KEY  ReadKeyStroke;
    EFI_EVENT           WaitForKey;
};

// ===================================================================
// Configuration Table (UEFI Spec 2.11, Table 4-8)
// ===================================================================

typedef struct {
    EFI_GUID  VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

// ===================================================================
// EFI_FILE_INFO (UEFI Spec 2.11, Section 13.5.16)
// ===================================================================

typedef struct {
    UINT64    Size;
    UINT64    FileSize;
    UINT64    PhysicalSize;
    EFI_TIME  CreateTime;
    EFI_TIME  LastAccessTime;
    EFI_TIME  ModificationTime;
    UINT64    Attribute;
    CHAR16    FileName[1];
} EFI_FILE_INFO;

// ===================================================================
// EFI_BOOT_SERVICES (UEFI Spec 2.11, Table 4-4)
// Slots used by AXL have proper typedefs.  Unused slots are void *.
// ===================================================================

typedef struct {
    EFI_TABLE_HEADER  Hdr;
    void  *RaiseTPL;
    void  *RestoreTPL;
    void  *AllocatePages;
    void  *FreePages;
    void  *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(
        IN  EFI_MEMORY_TYPE  PoolType,
        IN  UINTN            Size,
        OUT VOID           **Buffer
        );
    EFI_STATUS (EFIAPI *FreePool)(
        IN VOID  *Buffer
        );
    EFI_STATUS (EFIAPI *CreateEvent)(
        IN  UINT32            Type,
        IN  EFI_TPL           NotifyTpl,
        IN  EFI_EVENT_NOTIFY  NotifyFunction OPTIONAL,
        IN  VOID             *NotifyContext OPTIONAL,
        OUT EFI_EVENT        *Event
        );
    EFI_STATUS (EFIAPI *SetTimer)(
        IN EFI_EVENT        Event,
        IN EFI_TIMER_DELAY  Type,
        IN UINT64           TriggerTime
        );
    EFI_STATUS (EFIAPI *WaitForEvent)(
        IN  UINTN      NumberOfEvents,
        IN  EFI_EVENT *Event,
        OUT UINTN     *Index
        );
    EFI_STATUS (EFIAPI *SignalEvent)(IN EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(IN EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CheckEvent)(IN EFI_EVENT Event);
    void  *InstallProtocolInterface;
    void  *ReinstallProtocolInterface;
    void  *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(
        IN  EFI_HANDLE    Handle,
        IN  EFI_GUID     *Protocol,
        OUT VOID        **Interface
        );
    VOID  *Reserved;
    EFI_STATUS (EFIAPI *RegisterProtocolNotify)(
        IN  EFI_GUID   *Protocol,
        IN  EFI_EVENT   Event,
        OUT VOID      **Registration
        );
    void  *LocateHandle;
    void  *LocateDevicePath;
    void  *InstallConfigurationTable;
    void  *LoadImage;
    void  *StartImage;
    EFI_STATUS (EFIAPI *Exit)(
        IN EFI_HANDLE    ImageHandle,
        IN EFI_STATUS    ExitStatus,
        IN UINTN         ExitDataSize,
        IN CHAR16       *ExitData OPTIONAL
        );
    void  *UnloadImage;
    void  *ExitBootServices;
    void  *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(IN UINTN Microseconds);
    void  *SetWatchdogTimer;
    void  *ConnectController;
    void  *DisconnectController;
    void  *OpenProtocol;
    void  *CloseProtocol;
    void  *OpenProtocolInformation;
    void  *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(
        IN     EFI_LOCATE_SEARCH_TYPE  SearchType,
        IN     EFI_GUID              *Protocol OPTIONAL,
        IN     VOID                  *SearchKey OPTIONAL,
        IN OUT UINTN                 *NoHandles,
        OUT    EFI_HANDLE           **Buffer
        );
    EFI_STATUS (EFIAPI *LocateProtocol)(
        IN  EFI_GUID  *Protocol,
        IN  VOID      *Registration OPTIONAL,
        OUT VOID     **Interface
        );
    void  *InstallMultipleProtocolInterfaces;
    void  *UninstallMultipleProtocolInterfaces;
    void  *CalculateCrc32;
    void  *CopyMem;
    void  *SetMem;
    void  *CreateEventEx;
} EFI_BOOT_SERVICES;

// ===================================================================
// EFI_RUNTIME_SERVICES (UEFI Spec 2.11, Table 4-5)
// ===================================================================

typedef struct {
    EFI_TABLE_HEADER  Hdr;
    EFI_STATUS (EFIAPI *GetTime)(
        OUT EFI_TIME  *Time,
        OUT VOID      *Capabilities OPTIONAL
        );
    void  *SetTime;
    void  *GetWakeupTime;
    void  *SetWakeupTime;
    void  *SetVirtualAddressMap;
    void  *ConvertPointer;
    void  *GetVariable;
    void  *GetNextVariableName;
    void  *SetVariable;
    void  *GetNextHighMonotonicCount;
    void  *ResetSystem;
    void  *UpdateCapsule;
    void  *QueryCapsuleCapabilities;
    void  *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

// ===================================================================
// EFI_SYSTEM_TABLE (UEFI Spec 2.11, Table 4-1)
// ===================================================================

typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER                  Hdr;
    CHAR16                           *FirmwareVendor;
    UINT32                            FirmwareRevision;
    EFI_HANDLE                        ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *ConIn;
    EFI_HANDLE                        ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                        StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *StdErr;
    EFI_RUNTIME_SERVICES             *RuntimeServices;
    EFI_BOOT_SERVICES                *BootServices;
    UINTN                             NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE          *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// ===================================================================
// Utility
// ===================================================================

static inline int
axl_guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}
"""

# Extra names to add to preamble dedup that the line-by-line parser misses.
# ONLY include names that are actually defined in TYPES_H_CONTENT or
# CALLING_H_CONTENT above — adding names not in the preamble would
# cause their definitions to be silently dropped.
EXTRA_PREAMBLE_NAMES: set[str] = {
    # Console colors (in TYPES_H_CONTENT preamble)
    "EFI_BLACK", "EFI_BLUE", "EFI_GREEN", "EFI_CYAN", "EFI_RED",
    "EFI_MAGENTA", "EFI_BROWN", "EFI_LIGHTGRAY", "EFI_DARKGRAY",
    "EFI_LIGHTBLUE", "EFI_LIGHTGREEN", "EFI_LIGHTCYAN", "EFI_LIGHTRED",
    "EFI_LIGHTMAGENTA", "EFI_YELLOW", "EFI_WHITE", "EFI_BRIGHT",
    "EFI_TEXT_ATTR",
    "EFI_BACKGROUND_BLACK", "EFI_BACKGROUND_BLUE", "EFI_BACKGROUND_GREEN",
    "EFI_BACKGROUND_CYAN", "EFI_BACKGROUND_RED", "EFI_BACKGROUND_MAGENTA",
    "EFI_BACKGROUND_BROWN", "EFI_BACKGROUND_LIGHTGRAY",
    # File constants (in TYPES_H_CONTENT preamble)
    "EFI_FILE_MODE_READ", "EFI_FILE_MODE_WRITE", "EFI_FILE_MODE_CREATE",
    "EFI_FILE_DIRECTORY", "EFI_FILE_READ_ONLY",
    # Types defined in TYPES_H_CONTENT preamble
    "EFI_EVENT", "EFI_TPL", "EFI_EVENT_NOTIFY",
    "EFI_DEVICE_PATH_PROTOCOL", "EFI_MEMORY_DESCRIPTOR",
    "EFI_OPEN_PROTOCOL_INFORMATION_ENTRY", "EFI_CAPSULE_HEADER",
    # Core table types in preamble
    "EFI_TABLE_HEADER", "SIMPLE_TEXT_OUTPUT_MODE",
    "EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL", "EFI_TEXT_RESET",
    "EFI_TEXT_STRING", "EFI_TEXT_SET_ATTRIBUTE",
    "EFI_SIMPLE_TEXT_INPUT_PROTOCOL", "EFI_INPUT_RESET",
    "EFI_INPUT_READ_KEY", "EFI_CONFIGURATION_TABLE",
    "EFI_FILE_INFO",
    "EFI_BOOT_SERVICES", "EFI_RUNTIME_SERVICES", "EFI_SYSTEM_TABLE",
    # Opaque handles/IDs in preamble
    "EFI_HII_HANDLE", "EFI_STRING_ID", "EFI_IMAGE_ID",
    "EFI_FONT_HANDLE", "EFI_QUESTION_ID", "EFI_STRING",
    # Network protocols provided by axl-uefi-extra.h
    "EFI_SERVICE_BINDING_PROTOCOL",
    "EFI_SERVICE_BINDING_CREATE_CHILD", "EFI_SERVICE_BINDING_DESTROY_CHILD",
    "EFI_TCP4_PROTOCOL", "EFI_TCP4_ACCESS_POINT", "EFI_TCP4_OPTION",
    "EFI_TCP4_CONFIG_DATA", "EFI_TCP4_CONNECTION_STATE",
    "EFI_TCP4_COMPLETION_TOKEN", "EFI_TCP4_CONNECTION_TOKEN",
    "EFI_TCP4_LISTEN_TOKEN", "EFI_TCP4_FRAGMENT_DATA",
    "EFI_TCP4_RECEIVE_DATA", "EFI_TCP4_TRANSMIT_DATA",
    "EFI_TCP4_IO_TOKEN", "EFI_TCP4_CLOSE_TOKEN",
    "EFI_IP4_PROTOCOL", "EFI_IP4_ROUTE_TABLE",
    "EFI_IP4_CONFIG_DATA", "EFI_IP4_COMPLETION_TOKEN",
    "EFI_IP4_HEADER", "EFI_IP4_FRAGMENT_DATA",
    "EFI_IP4_RECEIVE_DATA", "EFI_IP4_OVERRIDE_DATA",
    "EFI_IP4_TRANSMIT_DATA", "EFI_IP4_MODE_DATA",
    "EFI_IP4_ICMP_TYPE",
    "EFI_IP4_CONFIG2_PROTOCOL", "EFI_IP4_CONFIG2_DATA_TYPE",
    "EFI_IP4_CONFIG2_INTERFACE_INFO",
    "EFI_DNS4_PROTOCOL", "EFI_DNS4_CONFIG_DATA",
    "EFI_DNS4_COMPLETION_TOKEN", "EFI_DNS4_HOST_TO_ADDR_DATA",
    "EFI_DNS4_ADDR_TO_HOST_DATA", "EFI_DNS4_RESOURCE_RECORD",
    "EFI_DNS4_GENERAL_LOOKUP_DATA", "EFI_DNS4_CACHE_ENTRY",
    "EFI_DNS4_MODE_DATA",
}


# ===================================================================
# Header file generation
# ===================================================================

# Explicit include order for all.h (dependency-correct)
ALL_H_ORDER: list[str] = [
    "calling.h",
    "types.h",
    "status.h",
    # Network headers early (TCP4/IP4/DNS4 used by AXL)
    "network.h",
    # Core headers
    "device-path.h",
    "console.h",
    "driver-model.h",
    "media.h",
    "tables.h",
    "image.h",
    # Remaining chapter headers (alphabetical)
    "acpi.h",
    "debug.h",
    "hii.h",
    "iscsi.h",
    "misc.h",
    "pci.h",
    "scsi.h",
    "secure.h",
    "usb.h",
    "guids.h",
]

# Per-header include dependencies beyond types.h + status.h
HEADER_DEPS: dict[str, list[str]] = {
    "tables.h": ["types.h", "status.h", "console.h"],
    "image.h": ["types.h", "status.h"],
}


def header_wrap(name: str, content: str, includes: list[str] | None = None,
                description: str = "") -> str:
    """Wrap content in a header file with guard and includes."""
    guard = f"AXL_UEFI_GEN_{name.upper().replace('-', '_').replace('.', '_')}_"
    lines: list[str] = []
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate UEFI headers from spec HTML")
    parser.add_argument("--input", "-i", type=Path, default=Path("deps/uefi-spec"))
    parser.add_argument("--output", "-o", type=Path,
                        default=Path("include/uefi/generated"))
    parser.add_argument("--dump", action="store_true",
                        help="Print extracted definitions to stdout")
    args = parser.parse_args()

    if not args.input.is_dir():
        print(f"Error: {args.input} not found. Run download-uefi-spec.py first.",
              file=sys.stderr)
        return 1

    output_dir = args.output
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect all type names from the static preamble for dedup
    preamble_names: set[str] = set(EXTRA_PREAMBLE_NAMES)
    for line in TYPES_H_CONTENT.split("\n"):
        # typedef ... NAME;
        m = re.search(r"}\s+(\w+)\s*;", line)
        if m:
            preamble_names.add(m.group(1))
        m = re.match(r"typedef\s+\w[\w\s*]*\s+(\w+)\s*;", line)
        if m:
            preamble_names.add(m.group(1))
        m = re.match(r"#define\s+(\w+)", line)
        if m:
            preamble_names.add(m.group(1))
        # enum members
        m = re.match(r"\s+(Efi\w+|Timer\w+|All\w+|By\w+|Allocate\w+|Max\w+|EFI_NATIVE)",
                     line)
        if m:
            preamble_names.add(m.group(1).rstrip(","))

    # Also extract struct closing names from multiline TYPES_H_CONTENT
    for m in re.finditer(r"}\s+(\w+)\s*;", TYPES_H_CONTENT):
        preamble_names.add(m.group(1))

    print(f"Preamble defines {len(preamble_names)} names")

    # Parse all chapters
    # key = output header name, value = list of cleaned_code strings
    headers: dict[str, list[str]] = {}
    seen_names: set[str] = set(preamble_names)
    guid_blocks: list[str] = []
    total_defs = 0
    total_skipped = 0

    html_files = sorted(args.input.glob("*.html"))

    for html_path in html_files:
        chapter = html_path.name

        if chapter in SKIP_CHAPTERS or chapter.startswith("Apx_"):
            continue
        if chapter.startswith("Frontmatter"):
            continue

        target = CHAPTER_MAP.get(chapter, "misc.h")

        p = PreExtractor()
        p.feed(html_path.read_text(encoding="utf-8", errors="replace"))

        for raw_block in p.blocks:
            cleaned = clean_code(raw_block)
            if not cleaned or not is_c_definition(cleaned):
                continue
            if not is_valid_block(cleaned):
                continue

            # Split multi-definition blocks
            sub_defs = split_block(cleaned)

            for sub_def in sub_defs:
                if not sub_def or not is_c_definition(sub_def):
                    continue
                if not is_valid_block(sub_def):
                    continue

                name = extract_name(sub_def)

                # GUID #defines go to guids.h
                if (name and "_GUID" in name
                        and "#define" in sub_def and "0x" in sub_def):
                    # Split multi-GUID blocks into individual defines
                    guid_subs = re.split(r"\n(?=#define\s)", sub_def)
                    for gs in guid_subs:
                        gs = gs.strip()
                        if not gs:
                            continue
                        gs_name = extract_name(gs)
                        if (gs_name and "_GUID" in gs_name
                                and gs_name not in seen_names):
                            seen_names.add(gs_name)
                            guid_blocks.append(gs)
                            total_defs += 1
                        else:
                            total_skipped += 1
                    continue

                # Dedup by name
                if name:
                    if name in seen_names:
                        total_skipped += 1
                        continue
                    seen_names.add(name)

                if target not in headers:
                    headers[target] = []
                headers[target].append(sub_def)
                total_defs += 1

    # Parse status codes from Appendix D
    status_content = parse_status_codes(args.input)

    if args.dump:
        print(f"\n=== Extracted {total_defs} definitions, "
              f"skipped {total_skipped} duplicates ===\n")
        print(f"GUIDs: {len(guid_blocks)}")
        for h, blocks in sorted(headers.items()):
            print(f"{h}: {len(blocks)} definitions")
        return 0

    # Write calling.h
    (output_dir / "calling.h").write_text(
        header_wrap("calling.h", CALLING_H_CONTENT,
                    description="UEFI calling convention macros."),
        encoding="utf-8")

    # Write types.h
    (output_dir / "types.h").write_text(
        header_wrap("types.h", TYPES_H_CONTENT,
                    description="Base UEFI types, constants, and enums."),
        encoding="utf-8")

    # Write status.h
    (output_dir / "status.h").write_text(
        header_wrap("status.h", status_content,
                    includes=["types.h"],
                    description="EFI status codes from Appendix D."),
        encoding="utf-8")

    # Write per-chapter headers (with reordered blocks)
    for hdr_name, blocks in sorted(headers.items()):
        ordered = reorder_blocks(blocks)
        content = "\n\n".join(ordered) + "\n"
        includes = HEADER_DEPS.get(hdr_name, ["types.h", "status.h"])
        (output_dir / hdr_name).write_text(
            header_wrap(hdr_name, content, includes=includes),
            encoding="utf-8")

    # Write guids.h
    guid_lines: list[str] = []
    guid_lines.append("#if defined(__GNUC__) || defined(__clang__)")
    guid_lines.append("#pragma GCC diagnostic push")
    guid_lines.append('#pragma GCC diagnostic ignored "-Wunused-variable"')
    guid_lines.append("#endif\n")

    for block in guid_blocks:
        # Convert #define NAME { ... } to static const EFI_GUID NAME = { ... };
        m = re.match(r"#define\s+(\w+)\s*\\?\s*(\{.+?\}\s*\})", block, re.DOTALL)
        if m:
            gname = m.group(1)
            gval = m.group(2).replace("\\", "").replace("\n", " ")
            gval = re.sub(r"\s+", " ", gval)
            guid_lines.append(f"static const EFI_GUID {gname} =")
            guid_lines.append(f"    {gval};\n")
        else:
            # Emit as-is (it's a #define)
            guid_lines.append(block + "\n")

    guid_lines.append("\n#if defined(__GNUC__) || defined(__clang__)")
    guid_lines.append("#pragma GCC diagnostic pop")
    guid_lines.append("#endif")

    (output_dir / "guids.h").write_text(
        header_wrap("guids.h", "\n".join(guid_lines),
                    includes=["types.h"],
                    description="All UEFI protocol GUIDs."),
        encoding="utf-8")

    # Collect global forward declarations from ALL headers
    global_fwd: list[str] = []
    global_fwd_seen: set[str] = set()
    for hdr_blocks in headers.values():
        for block in hdr_blocks:
            # Tagged structs
            m = re.search(r"typedef\s+struct\s+(\w+)\s*\{", block)
            if m:
                tag = m.group(1)
                m2 = re.search(r"}\s*(\w+)\s*;", block)
                if m2:
                    name = m2.group(1)
                    if name not in global_fwd_seen:
                        global_fwd_seen.add(name)
                        global_fwd.append(
                            f"typedef struct {tag} {name};")
                continue
            # Untagged structs
            m = re.search(r"typedef\s+struct\s*\{", block)
            if m:
                m2 = re.search(r"}\s*(\w+)\s*;", block)
                if m2:
                    name = m2.group(1)
                    if name not in global_fwd_seen:
                        global_fwd_seen.add(name)
                        global_fwd.append(
                            f"typedef struct _{name} {name};")
                continue
            # Unions
            m = re.search(r"typedef\s+union\s*\{", block)
            if m:
                m2 = re.search(r"}\s*(\w+)\s*;", block)
                if m2:
                    name = m2.group(1)
                    if name not in global_fwd_seen:
                        global_fwd_seen.add(name)
                        global_fwd.append(
                            f"typedef union _{name} {name};")

    # Write all.h umbrella with explicit include order
    all_includes: list[str] = []
    for inc in ALL_H_ORDER:
        if inc in ("calling.h", "types.h", "status.h", "guids.h"):
            all_includes.append(inc)
        elif inc in headers:
            all_includes.append(inc)

    all_lines: list[str] = []
    # First: base type headers
    all_lines.append('#include "calling.h"')
    all_lines.append('#include "types.h"')
    all_lines.append('#include "status.h"')
    all_lines.append("")
    # Global forward declarations (after base types, before chapter headers)
    all_lines.append("// Forward declarations for all struct/union types")
    all_lines.extend(global_fwd)
    all_lines.append("")
    # Chapter headers
    for inc in all_includes:
        if inc not in ("calling.h", "types.h", "status.h"):
            all_lines.append(f'#include "{inc}"')
    all_lines.append("")

    all_content = "\n".join(all_lines)
    (output_dir / "all.h").write_text(
        header_wrap("all.h", all_content,
                    description="Umbrella — includes all generated UEFI headers."),
        encoding="utf-8")

    # Summary
    hdr_count = 3 + len(headers) + 2  # calling + types + status + chapters + guids + all
    print(f"\nGenerated {hdr_count} header files ({total_defs} definitions, "
          f"{total_skipped} duplicates skipped):\n")
    print(f"  calling.h                    (static preamble)")
    print(f"  types.h                      (static preamble)")
    print(f"  status.h                     "
          f"{sum(1 for l in status_content.split(chr(10)) if l.startswith('#define'))}"
          f" status codes")
    for h in sorted(headers.keys()):
        print(f"  {h:<30s} {len(headers[h])} definitions")
    print(f"  guids.h                      {len(guid_blocks)} GUIDs")
    print(f"  all.h                        (umbrella)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
