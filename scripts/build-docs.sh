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
# Doxygen VERSION SKEW — local-clean does not imply CI-clean
#
# docs.yml installs whatever doxygen `ubuntu-latest`'s apt ships (1.9.8 at
# writing); a dev box is usually far newer. Reference resolution differs
# between them, and NOT in the direction you would hope: 1.13 resolved two
# \ref / explicit-link targets that 1.9.8 could not, so this gate reported
# clean locally while the v3.2.0 Docs run failed on both.
#
# There is no pin to add here — the docs job takes the distro's package. When a
# docs change matters, reproduce CI's exact version instead of trusting a newer
# local one:
#
#   podman run --rm -v "$PWD":/src:z -w /src ubuntu:24.04 bash -c \
#     'apt-get update -qq && apt-get install -y -qq doxygen && \
#      cd docs/sphinx && doxygen Doxyfile'
#
# Same shape as the clang-tidy container in docs/RELEASING.md, same reason.
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# Step 1: Doxygen → XML
# --------------------------------------------------------------------------

# The Doxyfile sets WARN_AS_ERROR=FAIL_ON_WARNINGS, so a dangling \ref, a
# half-documented parameter list or a malformed code span fails the build
# here rather than rotting until the next tagged release.
log_info "Running Doxygen (XML-only) ..."
mkdir -p "$OUT_DIR/doxygen-xml"
DOXY_LOG="$OUT_DIR/doxygen.log"
DOXY_RC=0
(cd "$SPHINX_DIR" && doxygen Doxyfile) 2>&1 | tee "$DOXY_LOG" || DOXY_RC=$?
DOXY_RC=${PIPESTATUS[0]:-$DOXY_RC}

# A doxygen too old to know FAIL_ON_WARNINGS does not fail -- it warns about the
# enum value, silently falls back to WARN_AS_ERROR=NO, and exits 0. The gate
# would be gone with nothing to show for it.
if grep -q "is not a valid enum value" "$DOXY_LOG"; then
    log_error "This doxygen ($(doxygen --version)) rejected a Doxyfile value and"
    log_error "fell back to a default — the zero-warning gate is NOT in effect."
    log_error "WARN_AS_ERROR=FAIL_ON_WARNINGS needs doxygen >= 1.9.6."
    exit 1
fi

# FAIL_ON_WARNINGS exits non-zero for WARNINGS. Doxygen also emits a class of
# diagnostic labelled `error:` -- "Found unknown command" is the one that turned
# up -- and those do NOT trip it: two of them passed this gate with rc 0 while
# it was calling itself zero-warning. Checked separately, because a gate that
# ignores the more severe of the two labels is worse than no gate.
#
# The usual cause is a backslash escape written in a doc comment: Doxygen reads
# `\v` as a command even inside a markdown code span, so it has to be `\\v`.
#
# NOT every error carries a file:line. A config-level one ("Included by graph
# for 'axl-macros.h' not generated, too many nodes") starts at column 0, and the
# file:line-anchored pattern this check used to have could not see it — so the
# v3.2.0 Docs run failed on an error class this gate reported clean. Match both
# shapes.
DOXY_ERR_RE='^([^ ]+:[0-9]+: )?error:'
if grep -qE "$DOXY_ERR_RE" "$DOXY_LOG"; then
    log_error "Doxygen reported ERRORS (below). These do not trip"
    log_error "WARN_AS_ERROR=FAIL_ON_WARNINGS, so they are checked separately."
    grep -E "$DOXY_ERR_RE" "$DOXY_LOG" | head -20 >&2
    exit 1
fi

if [[ $DOXY_RC -ne 0 ]]; then
    log_error "Doxygen reported doc problems (listed above as 'error:' because"
    log_error "WARN_AS_ERROR is on). The docs build is a zero-warning gate —"
    log_error "fix the header comments rather than silencing it."
    log_error ""
    log_error "Common causes and their fixes:"
    log_error "  'unable to resolve reference' -> the TARGET is not referenceable."
    log_error "      EXTRACT_ALL is NO, so only documented entities are indexed. Two"
    log_error "      cases: the target's header has no '@file' block (add one -- that"
    log_error "      alone makes every member in it referenceable), or the target"
    log_error "      itself carries no doc comment (give the #define / field a ///<)."
    log_error "      Do NOT 'fix' it by dropping the \\ref -- that hides a dead link."
    log_error "  \\ref Type immediately followed by ':' or '-' -> the punctuation is"
    log_error "      swallowed into the name; use '@ref Type \"Type\":'"
    log_error "  'parameter X is not documented' -> Doxygen only warns on a PARTIALLY"
    log_error "      documented list. Either the block mixes @param with ///< (once"
    log_error "      @param appears the ///< ones stop counting -- use ///< for all),"
    log_error "      or some params simply have no ///< yet."
    log_error ""
    log_error "Silent traps this gate CANNOT see (no warning is emitted):"
    log_error "  a code span whose content STARTS with an apostrophe -- \`'c'\` --"
    log_error "      desynchronizes every backtick after it; use @c 'c' instead"
    log_error "  a code span whose content ENDS with a backslash breaks the span"
    log_error "  '#AXL_FOO' never warns even when it does not resolve"
    exit 1
fi
log_success "Doxygen XML → $OUT_DIR/doxygen-xml/"

# --------------------------------------------------------------------------
# Step 2: Sphinx → HTML
# --------------------------------------------------------------------------

log_info "Building HTML (ReadTheDocs theme) ..."
sphinx-build -b html -q -W "$SPHINX_DIR" "$OUT_DIR/html"
log_success "HTML → $OUT_DIR/html/"

# --------------------------------------------------------------------------
# Step 3: Sphinx → man pages
# --------------------------------------------------------------------------

log_info "Building man pages ..."
sphinx-build -b man -q -W "$SPHINX_DIR" "$OUT_DIR/man"
log_success "Man pages → $OUT_DIR/man/"

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

echo ""
log_success "Documentation build complete."
echo "  HTML:  $OUT_DIR/html/index.html"
echo "  Man:   $OUT_DIR/man/"
