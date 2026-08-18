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
# Steps 2 and 3: Sphinx → HTML and man, together
#
# The two builders read the same sources and write different trees, so they
# have no reason to be sequential; and `-j auto` parallelises the reader phase
# within each. Measured on 8 cores: html 71s -> 28s, man 40s -> 12s, and run
# concurrently the pair costs what html alone costs. That takes this script,
# which is the SLOWEST job in verify.sh and therefore its wall-clock, from
# ~115s to ~30s.
#
# -W (warnings are errors) still bites under -j: verified by appending a
# dangling :ref: to index.rst and confirming the parallel build reports it and
# exits 1. A faster gate that stopped seeing would be a bad trade.
#
# Failures are collected rather than `set -e`'d one at a time, so a broken man
# build is still reported when html also fails — the point of the gate is to
# show every warning in one run, not the first.
#
# -E (never reuse the saved environment) IS LOAD-BEARING, for two reasons.
#
# THE CACHE GROWS WITHOUT BOUND. The `.. include:: ../../ROADMAP.md :parser:
# myst_parser.sphinx_` documents accumulate on every incremental rebuild
# instead of being replaced. Measured on this tree against a fresh build of the
# same sources:
#
#     .doctrees total          3.5 GB   vs    66 MB
#     environment.pickle      1712 MB   vs   9.9 MB     (173x)
#     guides/roadmap.doctree   818 MB   vs   317 KB     (2640x)   from 56 KB of md
#     guides/sdk.doctree       414 MB   vs   126 KB     (3360x)
#
# Each build then unpickles a bigger cache than the last, so it compounds: the
# pair peaked at 28.2 GB RSS and the OOM killer took sphinx-build twice on a
# 31 GB machine -- reported, before the attribution below was fixed, as
# "Sphinx reported warnings". With -E the html build is 669 MB / 32 s.
#
# AND A GATE MUST NOT TRUST A CACHE. This is the zero-warning gate; a saved
# environment that considers a file unchanged does not re-emit its warnings, so
# an incremental run can be green where a clean one is not. That is the "gate
# that cannot see" shape this tree keeps meeting, and -E is what closes it.
# Cost: nothing worth having. The full build is ~32 s, and it was never the
# incremental path that was fast.
# --------------------------------------------------------------------------

log_info "Building HTML (ReadTheDocs theme) + man pages ..."
SPHINX_RC=0

sphinx-build -b html -q -W -E -j auto "$SPHINX_DIR" "$OUT_DIR/html" \
    > "$OUT_DIR/sphinx-html.log" 2>&1 &
_html_pid=$!
sphinx-build -b man  -q -W -E -j auto "$SPHINX_DIR" "$OUT_DIR/man" \
    > "$OUT_DIR/sphinx-man.log" 2>&1 &
_man_pid=$!

# Keep the ACTUAL status of each, not a flattened 1. A shell reports a
# signal death as 128+signum, and telling that apart from a -W warning is the
# difference between "fix your docstring" and "this box ran out of memory" --
# see the attribution below.
_html_rc=0; _man_rc=0
wait $_html_pid || _html_rc=$?
wait $_man_pid  || _man_rc=$?
[[ $_html_rc -ne 0 || $_man_rc -ne 0 ]] && SPHINX_RC=1

# Always surface the output: -q means these are empty on success, so anything
# here is a diagnostic worth reading.
cat "$OUT_DIR/sphinx-html.log" "$OUT_DIR/sphinx-man.log" >&2

if [[ $SPHINX_RC -ne 0 ]]; then
    # A KILLED sphinx reported nothing at all, so blaming -W sends the reader
    # hunting a docstring warning that does not exist. It happened: two
    # `-j auto` builds run concurrently HERE, verify.sh runs this job
    # concurrently with both arch suites and clang-tidy, and the html worker
    # reached 17 GB anon-rss before the OOM killer took it -- reported as
    # "Sphinx reported warnings", which is the one thing it had not done.
    _signalled=""
    for _rc in $_html_rc $_man_rc; do
        [[ $_rc -gt 128 ]] && _signalled="$_signalled $((_rc - 128))"
    done
    if [[ -n "$_signalled" ]]; then
        log_error "Sphinx was KILLED by signal(s):$_signalled -- it did not"
        log_error "report anything. Signal 9 here is almost always the OOM"
        log_error "killer: two -j auto builds run concurrently above, and"
        log_error "verify.sh adds two QEMU suites and clang-tidy alongside."
        log_error "Check 'dmesg | grep -i oom', then retry with"
        log_error "'verify.sh --only=docs' to give it the machine."
    else
        log_error "Sphinx reported warnings (treated as errors by -W)."
    fi
    exit 1
fi

log_success "HTML → $OUT_DIR/html/"
log_success "Man pages → $OUT_DIR/man/"

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------

echo ""
log_success "Documentation build complete."
echo "  HTML:  $OUT_DIR/html/index.html"
echo "  Man:   $OUT_DIR/man/"
