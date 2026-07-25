#!/bin/bash
# test-discover-selftest.sh — host-only checks for lib/discover.sh (no QEMU).
# Not a QEMU integration test; excluded from discovery by name.
set -euo pipefail
cd "$(dirname "$0")"
source lib/discover.sh

fail=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1 (got '$2' want '$3')"; fail=1
    fi
}

# Fixture script with a full meta tag.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test-fixture-qemu.sh" <<'EOF'
#!/bin/bash
# test-meta: arch=both needs=swtpm,openssl est=42 local-only=1
EOF
check "arch field"        "$(test_meta_field "$tmp/test-fixture-qemu.sh" arch)"       "both"
check "needs field"       "$(test_meta_field "$tmp/test-fixture-qemu.sh" needs)"      "swtpm,openssl"
check "est field"         "$(test_meta_field "$tmp/test-fixture-qemu.sh" est)"        "42"
check "local-only field"  "$(test_meta_field "$tmp/test-fixture-qemu.sh" local-only)" "1"

# A script with no meta tag gets defaults.
cat > "$tmp/test-bare-qemu.sh" <<'EOF'
#!/bin/bash
echo hi
EOF
check "default arch"       "$(test_meta_field "$tmp/test-bare-qemu.sh" arch)"       "x64"
check "default est"        "$(test_meta_field "$tmp/test-bare-qemu.sh" est)"        "20"
check "default local-only" "$(test_meta_field "$tmp/test-bare-qemu.sh" local-only)" "0"

# test_port now lives in test-portalloc-selftest.sh, together with the
# allocator it is built on. The old "default base (standalone) == 18000"
# check went with it: a standalone run no longer has a fixed base, it claims
# a verified-free one. TEST_PORT_BASE, when set explicitly, is still exact
# arithmetic — that contract is asserted there.

[[ $fail -eq 0 ]] && echo "discover selftest: OK" || { echo "discover selftest: FAILED"; exit 1; }
