# shellcheck shell=bash
# stage-host-tools.sh -- the ONE rule for staging scripts/ into libexec/axl.
#
# WHY THIS FILE EXISTS. `axl` offers exactly the EXECUTABLE files in
# libexec/axl as commands (see scripts/axl's list_commands), so the mode bit
# is not a detail of the copy -- it IS the declaration "this is a command".
# Two paths stage the same file list: scripts/install.sh for a prefix, and
# scripts/make-host-tools-tarball.sh for the manager archive. Both read the
# list itself from the Makefile (`print-HOST_TOOL_FILES`) precisely so it
# cannot drift; the MODE rule was left in one of them and hand-approximated in
# the other, and it drifted exactly as a duplicated list would.
#
# What shipped in v4.7.0's host-tools tarball: everything 0755 with one
# hand-written exception, so `axl --help` offered `common` (axl-common.sh,
# which run-qemu.sh SOURCES -- a silent no-op as a command) and `gdb-sample`
# (gdb-sample.py, loaded INSIDE gdb, no shebang at all -- it died with a bash
# syntax error). Both are things a user would reasonably try.
#
# A SHEBANG IS THE MANIFEST. What decides is whether the file can be run at
# all: one with `#!` is a command and stages 755, one without is present only
# for its siblings to find and stages 644. That classifies every non-command
# from the file itself, so there is no second list of "which of these are
# commands" to keep in step with this one -- which is the whole point, given
# that is the mistake being fixed.
#
# NOT AN `install` WRAPPER, deliberately: the two callers differ on flags
# (install.sh passes -C to avoid mtime churn on an unchanged file, the tarball
# stages into a fresh temp tree), and a wrapper hiding that would trade one
# drift for another. This owns the RULE; the callers own the copy.
#
# Sourced, never executed -- and so it carries no shebang and is mode 0644,
# which is this file's own rule applied to itself. `# shellcheck shell=bash`
# stands in for the shebang shellcheck would otherwise read the dialect from.

# axl_host_tool_mode <file> -- print the mode this file must be staged with.
#
# NO PIPE. `head -c2 "$f" | grep -q '#!'` reads correctly today and is the
# shape that bites elsewhere in this tree: grep -q exits at the match and the
# upstream takes SIGPIPE, which under `set -o pipefail` reports failure for a
# match that SUCCEEDED. A comparison needs neither process.
axl_host_tool_mode() {
    if [[ "$(head -c2 "$1" 2>/dev/null)" == '#!' ]]; then
        printf '755'
    else
        printf '644'
    fi
}
