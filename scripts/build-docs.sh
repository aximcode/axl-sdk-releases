#!/bin/bash
# Build AXL API documentation (Sphinx + Breathe + Doxygen XML).
# Output: out/docs/html/  out/docs/man/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SPHINX_DIR="$ROOT_DIR/docs/sphinx"
OUT_DIR="$ROOT_DIR/out/docs"

source "$SCRIPT_DIR/axl-common.sh"

# --------------------------------------------------------------------------
# Check prerequisites
# --------------------------------------------------------------------------

MISSING=()

if ! command -v doxygen &>/dev/null; then
    MISSING+=("doxygen  (sudo dnf install doxygen)")
fi
if ! command -v sphinx-build &>/dev/null; then
    MISSING+=("sphinx   (pip3 install sphinx)")
fi
if ! python3 -c "import breathe" &>/dev/null; then
    MISSING+=("breathe  (pip3 install breathe)")
fi
if ! python3 -c "import sphinx_rtd_theme" &>/dev/null; then
    MISSING+=("sphinx-rtd-theme  (pip3 install sphinx-rtd-theme)")
fi
if ! python3 -c "import myst_parser" &>/dev/null; then
    MISSING+=("myst-parser  (pip3 install myst-parser)")
fi

if [[ ${#MISSING[@]} -gt 0 ]]; then
    log_error "Missing prerequisites:"
    for dep in "${MISSING[@]}"; do
        echo "  - $dep"
    done
    echo ""
    echo "Quick install:"
    echo "  sudo dnf install doxygen"
    echo "  pip3 install sphinx breathe sphinx-rtd-theme"
    exit 1
fi

# --------------------------------------------------------------------------
# Step 1: Doxygen → XML
# --------------------------------------------------------------------------

log_info "Running Doxygen (XML-only) ..."
mkdir -p "$OUT_DIR/doxygen-xml"
(cd "$SPHINX_DIR" && doxygen Doxyfile)
log_success "Doxygen XML → $OUT_DIR/doxygen-xml/"

# --------------------------------------------------------------------------
# Step 2: Sphinx → HTML
# --------------------------------------------------------------------------

log_info "Building HTML (ReadTheDocs theme) ..."
sphinx-build -b html -q "$SPHINX_DIR" "$OUT_DIR/html"
log_success "HTML → $OUT_DIR/html/"

# --------------------------------------------------------------------------
# Step 3: Sphinx → man pages
# --------------------------------------------------------------------------

log_info "Building man pages ..."
sphinx-build -b man -q "$SPHINX_DIR" "$OUT_DIR/man"
log_success "Man pages → $OUT_DIR/man/"

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

echo ""
log_success "Documentation build complete."
echo "  HTML:  $OUT_DIR/html/index.html"
echo "  Man:   $OUT_DIR/man/"
