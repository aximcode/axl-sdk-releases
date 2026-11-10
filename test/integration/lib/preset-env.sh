#!/bin/bash
# preset-env.sh -- clear what a CTest test preset put in this test's
# environment.
#
# A test preset's `environment` is set in ctest's own process, so EVERY test
# it runs inherits it (AXL_IT_TIMEOUT, AXL_IT_CACHE, AXL_PREP_SKIP_LOCAL, ...).
# A test that itself drives ctest, the prep, or a stub of either then measures
# the outer run's settings instead of its own. That happened twice in one
# change: under `ctest --preset x64-ci`, test-release-suite.sh's stub recorded
# the inherited AXL_IT_CACHE=0, and a test-ctest-integration.sh control was
# fed AXL_PREP_SKIP_LOCAL=1.
#
# So such a test clears every key ANY test preset sets, read from
# CMakePresets.json itself, plus every AXL_IT_* / AXL_PREP_* a front end may
# have set (run-integration.sh, verify.sh), so a key added later cannot leak
# the same way unnoticed.
#
# Usage (sourced): preset_env_unset PROJECT_DIR
#   Returns non-zero, naming why, when the presets cannot be read: a scrub
#   that silently cleared nothing is worse than none.

preset_env_unset() {
    local presets="$1/CMakePresets.json" keys k
    if ! keys=$(jq -r '[.testPresets[]?.environment // {} | keys[]] | unique | .[]' "$presets" 2>&1); then
        echo "preset-env.sh: cannot read the test presets' environment from $presets: $keys" >&2
        return 1
    fi
    for k in $keys; do
        unset "$k"
    done
    # ...and whatever a FRONT END put in ctest's environment for its run
    # (AXL_IT_LOGDIR, AXL_PREP_TESTS, ...): no preset names those, and a test
    # that starts a nested ctest would otherwise run the outer run's prep
    # into the outer run's log directory. By prefix, so a knob added later
    # cannot leak the same way.
    for k in "${!AXL_IT_@}" "${!AXL_PREP_@}"; do
        unset "$k"
    done
}
