# shellcheck shell=bash
# profile.sh — where the integration suite's wall clock actually goes.
#
# Sourced by common-test.sh. **Inert unless AXL_TEST_PROFILE names a file**,
# so a normal run pays a handful of string comparisons and nothing else. That
# matters more than usual here: this measures a suite whose failures are
# already timing-sensitive, so instrumentation that changed the timing would
# be measuring itself.
#
# WHY BOOTS AND NOT TESTS. run-integration.sh already reports per-test wall
# time, and that was enough to find the top of the list and no further. The
# unit of cost in a QEMU suite is a GUEST BOOT -- firmware init dominates, and
# a test's duration is mostly a count of them. test-console-device-qemu.sh is
# 379 s because it runs 13 DEBUG-OVMF boots SERIALLY inside one test, and the
# -j6 pool parallelises across tests and never within one, so those 13 are
# pinned to a single worker. Per-test timing cannot show that; boots per test
# and seconds per boot show it immediately, and show whether any other test is
# in the same shape.
#
# It also records what each test STAGED, with digests. That is the input to
# the association question (AXL-CI-Release-Speed-Design.md §12.6): which tests
# can a given change possibly affect? Derived from what a test actually
# stages, never from a directory name -- see §12.5 for why a name-based map is
# unsafe in this tree.
#
# Format is one record per line, `|`-separated, appended with a single >>
# per record so concurrent workers under -jN interleave whole lines rather
# than tearing them:
#
#   boot|<test>|<seconds>|<label>     one guest boot, wall seconds
#   efi|<test>|<dest>|<sha256>        an artifact staged into the guest image
#
# Per-test totals come from run-integration.sh, which already measures them.
#
# Usage:
#   AXL_TEST_PROFILE=/tmp/prof.txt ./test/integration/run-integration.sh --arch X64
#   python3 test/integration/lib/profile-report.py /tmp/prof.txt

_prof_active() { [[ -n "${AXL_TEST_PROFILE:-}" ]]; }

# Monotonic-ish seconds with millisecond resolution. `date +%s.%N` rather than
# SECONDS: boots are tens of seconds but the setup phases are sub-second, and
# integer SECONDS would round the interesting ones to 0.
_prof_now() { date +%s.%N; }

_prof_emit() {
    _prof_active || return 0
    printf '%s\n' "$*" >> "$AXL_TEST_PROFILE"
}

# Name a record by the TEST SCRIPT, not by $0's path, so the report keys match
# what run-integration.sh prints and what discover_tests emits.
_prof_test_name() { basename "${BASH_SOURCE[-1]:-$0}"; }

_prof_elapsed() {   # $1 = start stamp
    _prof_active || return 0
    awk -v a="$1" -v b="$(_prof_now)" 'BEGIN { printf "%.3f", b - a }'
}

# KNOWN GAP, recorded rather than left for someone to rediscover as a "fast"
# test: test-crashhandler.sh assembles its own QEMU command
# (build_crash_qemu_cmd, which strips KVM because the crash handler needs a real
# #GP) and launches it directly, reaching neither launcher below nor
# run-qemu.sh. Its ~42 s and 3 boots are therefore absent from the boot columns.
# One bespoke test; hooking it would mean instrumenting a private command
# assembly, which is a worse trade than saying so here.

# --- hooks, called from common-test.sh --------------------------------------

_prof_boot_start() {
    _prof_active || return 0
    _PROF_BOOT_AT=$(_prof_now)
}

# $1 = optional label distinguishing boots within one test (scenario name).
# Without one they are still counted; the label only makes the report readable.
_prof_boot_end() {
    _prof_active || return 0
    [[ -n "${_PROF_BOOT_AT:-}" ]] || return 0
    _prof_emit "boot|$(_prof_test_name)|$(_prof_elapsed "$_PROF_BOOT_AT")|${1:-unlabelled}"
    unset _PROF_BOOT_AT
}

# $1 = source path, $2 = destination inside the guest image.
# Digest the SOURCE: it is the thing a rebuild changes, and it is what a cache
# would key on. sha256sum is ~5 ms on a 50 KB .efi; skipped entirely when the
# profile is off.
_prof_efi() {
    _prof_active || return 0
    local sha
    sha=$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1)
    _prof_emit "efi|$(_prof_test_name)|$2|${sha:-unreadable}"
}

# NO per-test total is recorded here, and NO EXIT trap is installed, which is
# deliberate on both counts:
#
#   - run-integration.sh already times every test and prints it, so a `test|`
#     record would be a second copy of a number that already exists -- and two
#     copies of one measurement is how §12.1 ended up quoting 3,396 s and
#     3,385 s for the same quantity.
#   - `trap ... EXIT` is not composable. Tests install their own EXIT traps to
#     remove work directories (`trap 'rm -rf "$WORK"' EXIT` appears in several),
#     and bash keeps ONE handler per signal, so instrumenting via EXIT would
#     silently disable their cleanup. Profiling that leaks a 40 MB image per
#     test would cost more than it measures.
