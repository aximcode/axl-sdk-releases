#!/bin/bash
# discover.sh — enumerate runnable integration tests + read their
# `# test-meta:` headers. Sourced by run-integration.sh and the meta lint.
#
# A header line looks like:
#   # test-meta: arch=x64 needs=swtpm,openssl est=12 local-only=0
# Fields (all optional; defaults in test_meta_field):
#   arch        x64 | aa64 | both   — which arch(es) the test runs on
#   needs       comma list          — host apt deps / tools the test requires
#   est         integer seconds      — wall-clock estimate, for shard balancing
#   local-only  0 | 1                — 1 = excluded from CI (needs a capability
#                                       the GitHub runners lack, e.g. QMP
#                                       pointer injection)

# Echo one `# test-meta:` field (or its default) for a test script.
test_meta_field() {
    local script="$1" field="$2" line val
    line=$(grep -m1 '^# test-meta:' "$script" 2>/dev/null || true)
    # token after `field=`, up to the next space
    val=$(printf '%s\n' "$line" | grep -oE "${field}=[^ ]+" | head -n1 | cut -d= -f2-)
    if [[ -n "$val" ]]; then printf '%s\n' "$val"; return; fi
    case "$field" in
        arch)       printf 'x64\n' ;;
        needs)      printf '\n' ;;
        est)        printf '20\n' ;;
        local-only) printf '0\n' ;;
        *)          printf '\n' ;;
    esac
}

# Print runnable test scripts, one per line, filtered by arch + local-only.
discover_tests() {
    local want_arch="X64" include_local=0 only_local=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch) want_arch="$2"; shift 2 ;;
            --include-local-only) include_local=1; shift ;;
            # The INVERSE of what --ci selects: ONLY the tests a CI runner
            # structurally cannot run. Measured 2026-08-19, X64: 14 tests and
            # 733 s of the suite's 3,385 s. The other 2,652 s is work CI
            # repeats on every push to main, on our own box, for free -- so
            # for an inner-loop run it is the 78% that buys nothing a push
            # would not tell you minutes later. Implies --include-local-only,
            # since asking for only local-only tests and then filtering them
            # out would select nothing.
            --only-local) only_local=1; include_local=1; shift ;;
            *) shift ;;
        esac
    done
    local dir; dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    local f base arch lo
    for f in "$dir"/test-*.sh; do
        base=$(basename "$f")
        # Skip the unit batch runner, the aggregate runner, and the self-tests.
        case "$base" in
            test-axl.sh|test-all.sh|test-*-selftest.sh) continue ;;
        esac
        arch=$(test_meta_field "$f" arch)
        lo=$(test_meta_field "$f" local-only)
        [[ "$lo" == "1" && $include_local -eq 0 ]] && continue
        [[ "$lo" != "1" && $only_local -eq 1 ]] && continue
        case "$arch" in
            both) : ;;
            x64)    [[ "$want_arch" == "X64" ]] || continue ;;
            aa64)   [[ "$want_arch" == "AARCH64" ]] || continue ;;
        esac
        printf '%s\n' "$f"
    done
}

# `discover.sh --lint` — fail (and name offenders) if any QEMU integration test
# lacks a `# test-meta:` header, so a new test can't silently escape discovery /
# sharding. Run from the lint job + `make check-test-meta`.
if [[ "${BASH_SOURCE[0]}" == "${0}" && "${1:-}" == "--lint" ]]; then
    _d="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    _miss=0
    for _f in "$_d"/test-*.sh; do
        case "$(basename "$_f")" in
            test-axl.sh|test-all.sh|test-*-selftest.sh) continue ;;
        esac
        grep -q '^# test-meta:' "$_f" || { echo "missing test-meta: $_f"; _miss=1; }
    done
    [[ $_miss -eq 0 ]] && echo "check-test-meta: clean ($(discover_tests --include-local-only | wc -l) tests)"
    exit $_miss
fi
