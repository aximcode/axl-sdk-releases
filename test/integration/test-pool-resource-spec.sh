#!/bin/bash
# test-meta: arch=none needs=cmake est=15 local-only=0
# test-pool-resource-spec.sh -- `python3 -m axlpool resource-spec`: the CTest
# resource spec (format 1.0) of a tree's guest classes, one resource type
# per class and arch (`guest_<class>_<arch>`), its slots the class's. Pins
# the exact JSON, the refusals, and that CTest itself reads it: a test
# asking for one slot gets one, a test asking for more than exist is not
# run. Boots nothing.
#
# Usage: ./test/integration/test-pool-resource-spec.sh

set -uo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
POOL_DIR="$PROJECT_DIR/test/pool"
export TEST_SKIP_RATCHET=1

WORK="$(cd -P "$(mktemp -d -t axl-poolr.XXXXXXXX)" && pwd)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
spec() {    # spec ARGS... -> the generator, run from the pool directory
    (cd "$POOL_DIR" && python3 -m axlpool resource-spec "$@")
}
fixture() { # fixture NAME TOML -> a tree whose classes.toml is TOML
    mkdir -p "$WORK/$1/test/pool"
    printf '%s\n' "$2" > "$WORK/$1/test/pool/classes.toml"
}

echo "=== guest pool: CTest resource spec ==="

out="$(spec --tree "$PROJECT_DIR" 2>&1)"
rc=$?
# The version, then each resource type and its slots (the JSON's own shape is
# pinned by the CTest half below, which reads it).
check "this tree's classes: exit 0, the exact spec" "$rc
$(python3 -c 'import json, sys; d = json.load(sys.stdin); print(d["version"]); [print(k, v) for k, v in sorted(d["local"][0].items())]' <<< "$out" 2>&1)" \
"0
{'major': 1, 'minor': 0}
guest_ata_x64 [{'id': '0', 'slots': 4}]
guest_fixture_net_x64 [{'id': '0', 'slots': 4}]
guest_gpu_aa64 [{'id': '0', 'slots': 4}]
guest_gpu_x64 [{'id': '0', 'slots': 4}]
guest_ipmi_kcs_x64 [{'id': '0', 'slots': 8}]
guest_ipmi_ssif_x64 [{'id': '0', 'slots': 8}]
guest_mac_replay_x64 [{'id': '0', 'slots': 4}]
guest_plain_aa64 [{'id': '0', 'slots': 4}]
guest_plain_firmware_aa64 [{'id': '0', 'slots': 4}]
guest_plain_firmware_x64 [{'id': '0', 'slots': 4}]
guest_plain_x64 [{'id': '0', 'slots': 4}]
guest_scsi_x64 [{'id': '0', 'slots': 4}]
guest_smp4_aa64 [{'id': '0', 'slots': 4}]
guest_smp4_x64 [{'id': '0', 'slots': 4}]
guest_standard_aa64 [{'id': '0', 'slots': 8}]
guest_standard_x64 [{'id': '0', 'slots': 8}]
guest_unit_aa64 [{'id': '0', 'slots': 4}]
guest_unit_x64 [{'id': '0', 'slots': 4}]"

# A class name CTest cannot spell is refused where the classes load (the CMake
# registration names the resource type with it verbatim), so no two classes
# can normalise to one resource type.
fixture names '[class.my-net]
arches = ["X64"]
slots = 2'
out="$(spec --tree "$WORK/names" 2>&1)"
check "a class name CTest cannot spell: refused (exit 1) with the loader's message" "$? $out" \
    "1 axlpool resource-spec: $WORK/names/test/pool/classes.toml: class \`my-net\`: a class name is lower-case letters, digits and underscores, starting with a letter (it names a CTest resource type)"

fixture bad '[class.x]
slots = "four"'
out="$(spec --tree "$WORK/bad" 2>&1)"
rc=$?
check "malformed classes: refused (exit 1) with the loader's own message, no traceback" \
    "$rc $([[ "$out" == "axlpool resource-spec: "*classes.toml* ]] && echo named) $(grep -c Traceback <<< "$out")" \
    "1 named 0"

out="$(spec --tree "$WORK/none" 2>&1)"
check "a tree with no classes.toml: refused (exit 1)" "$? $out" \
    "1 axlpool resource-spec: $WORK/none has no test/pool/classes.toml"

# CTest itself: does it read the spec, and allocate by it?
if command -v cmake > /dev/null && command -v ctest > /dev/null; then
    spec --tree "$PROJECT_DIR" > "$WORK/spec.json"
    mkdir -p "$WORK/proj"
    cat > "$WORK/proj/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(spec NONE)
enable_testing()
add_test(NAME one COMMAND sh -c "echo slot=$CTEST_RESOURCE_GROUP_0_GUEST_STANDARD_X64")
set_tests_properties(one PROPERTIES RESOURCE_GROUPS "guest_standard_x64:1")
add_test(NAME too_many COMMAND true)
set_tests_properties(too_many PROPERTIES RESOURCE_GROUPS "guest_standard_x64:9")
EOF
    cmake -S "$WORK/proj" -B "$WORK/build" > "$WORK/cmake.log" 2>&1
    (cd "$WORK/build" && ctest --resource-spec-file "$WORK/spec.json" -V > "$WORK/ctest.log" 2>&1)
    check "ctest: a test asking one slot gets one (id 0, 1 slot)" \
        "$(grep -o 'slot=id:0,slots:1' "$WORK/ctest.log" | head -1)" "slot=id:0,slots:1"
    check "ctest: a test asking more slots than the spec has is not run" \
        "$(grep -c 'Insufficient resources' "$WORK/ctest.log")" "1"
else
    echo "  SKIP: ctest: a test asking one slot gets one (no cmake/ctest)"
    echo "  SKIP: ctest: a test asking more slots than the spec has is not run (no cmake/ctest)"
fi

echo "pool-resource-spec: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
