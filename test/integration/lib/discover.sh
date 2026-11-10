#!/bin/bash
# discover.sh — enumerate runnable integration tests + read their
# `# test-meta:` headers. Sourced by run-integration.sh and the meta lint.
#
# A header line looks like:
#   # test-meta: arch=x64 needs=swtpm,openssl est=12 local-only=0
# Fields (all optional; defaults in test_meta_field):
#   arch        x64 | aa64 | both   — which arch(es) the test runs on
#   needs       comma list          — host apt deps / tools the test requires
#   est         integer seconds      — wall-clock estimate, for shard balancing,
#                                       and CTest's COST for a test the tree's
#                                       measured cost data does not know yet
#   cpu         integer 1..8         — the CPUs the test keeps busy (default
#                                       1): CTest's PROCESSORS, charged against
#                                       -j, which is a CPU budget. Set from
#                                       MEASUREMENT -- the test's CPU-seconds
#                                       over its wall time, rounded up -- for a
#                                       test that compiles (install.sh, cmake
#                                       or ninja of a library or SDK build); a
#                                       booted guest and a host script keep 1
#   local-only  0 | 1                — 1 = excluded from CI (needs a capability
#                                       the GitHub runners lack, e.g. QMP
#                                       pointer injection)
#   builds      comma list          — the scripts/axl-build.sh arguments this
#                                     test uses to build its OWN fixtures,
#                                     beyond the ones the runner already
#                                     pre-builds ($TEST_PREBUILT_TARGETS). A
#                                     `VAR=VALUE` entry is passed through
#                                     (BUILD=RELEASE); `ARCH=x64` overrides
#                                     the run's arch. See `--lint` below and
#                                     lib/suite-prep.sh's fixture pre-build.
#                                     (`build-backend=` named make or cmake
#                                     until the Makefile was retired in 5.0.0;
#                                     `--lint` refuses it now.)
#   size-gate   comma list          — goals whose SIZE this test caps.
#                                     Each must also be a builds= target (the
#                                     test cannot cap an image it does not
#                                     build); see `--lint` below.
#   size-gate-paths comma list      — path PREFIXES of every file that can
#                                     change those targets' bytes
#                                     (`src/,deps/,tools/sshd,cmake/`).
#                                     scripts/which-gates.sh names the test as
#                                     covering any changed path under one --
#                                     a literal match, no build and no cache
#                                     (size_gate_covers). Required with
#                                     size-gate=. `gate.check-size-gate-paths`
#                                     derives the real set from the build tree
#                                     (scripts/size-gate-inputs.sh) and fails
#                                     when a linked file lies outside them.
#   size-gate-exclude comma list    — globs (`*.md`) a change under those
#                                     prefixes is NOT routed for: prose under
#                                     src/ cannot move an image. The drift
#                                     check fails if one matches any file the
#                                     build links, so an exclusion can
#                                     never hide an input.
#   guest       a class name        — `run-integration.sh --pool` runs the test
#                                     on a pooled guest of this class
#                                     (test/pool/classes.toml) under `axlpool
#                                     run` instead of booting one. Requires a
#                                     checked-in <TestName>.nsh run through
#                                     test_use_nsh (common-test.sh, "Pooled
#                                     mode"), a script reaching
#                                     test_use_nsh_text, or run-qemu.sh
#                                     calls; `--lint` holds it to one.
#               `none`              — the test runs a guest launcher (real,
#                                     stubbed or dry-run) and is deliberately
#                                     off the pool (the boot is its subject,
#                                     its machine is one no class gives, or
#                                     it is not converted yet); a
#                                     `# guest=none: <why>` header comment
#                                     says why. `--lint` fails a test that
#                                     launches a guest with NO guest=, so
#                                     none stays booted undecided.
#
# A field may live on any `# test-meta:` line, not only the first: `builds=`
# for a test with nine fixtures is longer than the rest of the header put
# together, and a 200-column header line is a header nobody reads.

# The targets run-integration.sh builds ONCE, up front, for every run. Kept
# here rather than in the runner because `--lint` has to know the same set:
# a test declaring one of these in builds= would make each run relink `all`
# a second time for nothing.
TEST_PREBUILT_TARGETS="all tests tools axl-busybox"

# cpu= is a CPU count, 1 to this. A test asking for more than -j is run alone
# (ctest clamps it), so the cap only keeps a typo from serialising the suite.
TEST_META_CPU_MAX=8

# suite_cpu_slots -- the integration suite's default -j: the CPU budget each
# test draws its cpu= from (CTest's PROCESSORS), for every front end that runs
# the suite (run-integration.sh, scripts/release-suite.sh). More slots than
# CPUs, because a booted guest keeps about half a CPU busy: the suite's tests
# averaged 0.48 CPU-seconds per second of wall, measured 2026-11-07
# (docs/AXL-CI-Release-Speed-Design.md §15, which also has the -j sweep).
suite_cpu_slots() {
    local n; n=$(nproc 2>/dev/null || echo 1)
    printf '%s\n' "$(( n + 4 ))"
}

# Echo one `# test-meta:` field (or its default) for a test script.
test_meta_field() {
    local script="$1" field="$2" l val="" re="(^|[[:space:]])${2}=([^ ]+)"
    # THE HEADER, NOT THE FILE. Dropping the stop at the first code line to
    # allow a second header line would otherwise scan every `^# test-meta:`
    # line anywhere -- and a fixture script written into a HEREDOC then
    # supplies a ghost `local-only=1`, which drops the real test out of the CI
    # selection silently. So: the contiguous comment block at the top, and
    # nothing after the first line of actual code. The field is the token
    # after `field=`, up to the next space, anchored at a word boundary:
    # unanchored, asking for `est` matched the `est=` inside
    # `builds=manifest=1,...` and returned that instead. The first header
    # line naming it wins.
    #
    # PURE BASH: discovery asks this three times for each of ~300 tests, and
    # the awk|grep|grep|head|sed|cut pipeline it replaced cost ~6,000 forks
    # -- 3.5-7 s per discovery, paid by every runner start, which-gates call
    # and --lint (the discover self-test called --lint ~18 times: 80 of its
    # 91 s). Same answer on every field of every test, compared.
    if [[ -f "$script" && -r "$script" ]]; then
        while IFS= read -r l || [[ -n "$l" ]]; do
            [[ "$l" =~ ^[[:space:]]*($|#) ]] || break
            [[ "$l" == "# test-meta:"* && "$l" =~ $re ]] && { val="${BASH_REMATCH[2]}"; break; }
        done < "$script"
    fi
    if [[ -n "$val" ]]; then printf '%s\n' "$val"; return; fi
    case "$field" in
        arch)       printf 'x64\n' ;;
        needs)      printf '\n' ;;
        est)        printf '20\n' ;;
        cpu)        printf '1\n' ;;
        local-only) printf '0\n' ;;
        *)          printf '\n' ;;
    esac
}

# test_meta_pool_class <script> -- the pool class the test runs on, or
# nothing: no guest= at all, or `guest=none` (the test boots its own guest on
# purpose). Every consumer that leases, labels or registers by class asks
# this, never test_meta_field directly, so `none` is never taken for a class.
test_meta_pool_class() {
    local g; g=$(test_meta_field "$1" guest)
    [[ "$g" == none ]] || printf '%s\n' "$g"
}

# test_meta_guest_none_reason <script> -- the text of the header's
# `# guest=none: <why>` comment (the first one), or nothing. Read from the
# same header block test_meta_field reads, for the same reason.
test_meta_guest_none_reason() {
    local l re='^#[[:space:]]*guest=none:[[:space:]]*(.*[^[:space:]])[[:space:]]*$'
    [[ -f "$1" && -r "$1" ]] || return 0
    while IFS= read -r l || [[ -n "$l" ]]; do
        [[ "$l" =~ ^[[:space:]]*($|#) ]] || break
        [[ "$l" =~ $re ]] && { printf '%s\n' "${BASH_REMATCH[1]}"; return 0; }
    done < "$1"
}

# test_sources <script> <lint dir> -- the script and every lib/*.sh it
# sources, transitively (each once), one per line: what runs when it runs.
test_sources() {
    local srcs=("$1") i=0 lib
    while [[ $i -lt ${#srcs[@]} ]]; do
        while IFS= read -r lib; do
            lib="$2/lib/$lib"
            [[ -f "$lib" && " ${srcs[*]} " != *" $lib "* ]] && srcs+=("$lib")
        done < <(sed -nE 's|^[[:space:]]*(source\|\.)[[:space:]]+"?\$(\(dirname "\$0"\)\|\{?SCRIPT_DIR\}?\|\{?TESTS_DIR\}?)/lib/([A-Za-z0-9_.-]+\.sh)"?.*|\3|p' "${srcs[$i]}")
        i=$((i + 1))
    done
    printf '%s\n' "${srcs[@]}"
}

# What starts a guest, in a test's own code (comment lines never count):
# common-test.sh's runs, the timing hook a direct launcher wraps itself in,
# scripts/run-qemu.sh and the shipped scripts that wrap it (ssh-demo.sh,
# axl-emulate, profile-qemu.sh, show-it.sh, console-visual.py) -- as a
# "$DIR/scripts/<name>" word, or through a RUN_QEMU variable; not a path some
# command merely writes or hashes, or a word in a list or a message -- QEMU
# itself, and the pool's own case files, which start real guests through
# axlpool. A superset of what lib/timing-report.py sorts a log by (it needs
# only the instrumented launchers). A launcher counts wherever it is written
# in a sourced lib, called or not: the guest=<class> branch's stricter
# "reached function" rule is about WHICH way onto a lease, not whether a
# guest is started at all.
TEST_GUEST_LAUNCH_RE='(^|[^A-Za-z0-9_])(test_run_foreground|test_run_background|test_timing_begin)([^A-Za-z0-9_]|$)|"\$\{?[A-Za-z_][A-Za-z0-9_]*\}?/scripts/(run-qemu\.sh|ssh-demo\.sh|axl-emulate|profile-qemu\.sh|show-it\.sh|console-visual/console-visual\.py)"|\$\{?RUN_QEMU|qemu-system-|"\$\{TEST_QEMU_CMD\[@\]\}"|"\$TEST_QEMU_BIN"|"\$QEMU_BIN"|test/pool/[a-z]+-cases\.py'

# test_launches_guest <file>... -- does any of these files (a test and the
# libs it sources) start a guest?
test_launches_guest() {
    # Not `a | grep -q`: under pipefail the early exit fails it ON A MATCH.
    # discover.sh itself is skipped: the RE above would match its own
    # definition, making every test that sources it "launch".
    local f files=()
    for f in "$@"; do [[ "$(basename "$f")" == discover.sh ]] || files+=("$f"); done
    [[ ${#files[@]} -gt 0 ]] || return 1
    grep -qE "$TEST_GUEST_LAUNCH_RE" < <(grep -hvE '^[[:space:]]*#' "${files[@]}")
}

# size_gate_paths <script> -- its declared size-gate-paths= prefixes, one
# per line.
size_gate_paths() {
    test_meta_field "$1" size-gate-paths | tr ',' '\n' | grep -v '^$' || true
}

# size_gate_covers <script> <repo-relative path> -- is the path under one of
# the script's size-gate-paths= prefixes? A plain string-prefix test, so
# `deps/` covers the bare gitlink path a submodule bump reports, and
# `tools/sshd` covers tools/sshd-svc.c. One definition, shared by the router
# (which-gates.sh) and the drift check (size-gate-inputs.sh --check), so the
# check grades the very predicate that routes.
size_gate_covers() {
    local p
    while IFS= read -r p; do
        [[ -n "$p" && "$2" == "$p"* ]] && return 0
    done < <(size_gate_paths "$1")
    return 1
}

# size_gate_excluded <script> <path> -- does the path match one of the
# script's size-gate-exclude= globs? Shell pattern match: `*` crosses `/`,
# so `*.md` matches src/loop/README.md.
size_gate_excluded() {
    local g
    while IFS= read -r g; do
        # shellcheck disable=SC2053  # $g is a glob on purpose
        [[ -n "$g" && "$2" == $g ]] && return 0
    done < <(test_meta_field "$1" size-gate-exclude | tr ',' '\n')
    return 1
}

# size_gate_routes <script> <path> -- would which-gates send a change to the
# path to this size-gated test? Under a prefix and not excluded. The router
# asks exactly this; the drift check asks it of every linked file.
size_gate_routes() {
    size_gate_covers "$1" "$2" && ! size_gate_excluded "$1" "$2"
}

# discover_test_basename <path> -- "test-foo" for any spelling of the file
# (dir and .sh stripped). One definition, so a comparison of a covering file
# against discovery's output cannot disagree about what the name is.
discover_test_basename() {
    local b="${1##*/}"
    printf '%s\n' "${b%.sh}"
}

# discover_is_test_name <path-or-name> -- does the NAME alone qualify as a
# runnable integration test? Not test-axl.sh (verify.sh's), not test-all.sh,
# not a *-selftest.sh (scripts/run-selftests.sh's), and it must be test-*.sh.
# This is the whole of discovery's name filter, shared so that callers who only
# need the name question (which-gates, deciding which covering files to list)
# do not run the arch/local-only pass over every test's header. The arch= and
# local-only= gates remain discover_tests's, and the runner's, to apply.
discover_is_test_name() {
    local b; b="${1##*/}"
    # A path must be directly in test/integration: lib/test-cache.sh matches
    # test-*.sh by name and is a helper. A bare name has no directory to judge.
    if [[ "$1" == */* ]]; then
        local d="${1%/*}"
        [[ "$d" == test/integration || "$d" == */test/integration ]] || return 1
    fi
    case "$b" in
        test-axl.sh|test-all.sh|test-*-selftest.sh) return 1 ;;
        test-*.sh) return 0 ;;
    esac
    return 1
}

# discover_drop_explanation [arch] [local-only-note] -- why a covering file can
# be absent from a run's selection. Printed by run-integration.sh --for (where
# arch/local-only matter) and by which-gates (where only the name does), so the
# two cannot give different reasons for the same drop.
discover_drop_explanation() {
    local arch="${1:-}" lo="${2:-}" who="discovery"
    [[ -n "$arch" ]] && who="discovery (--arch $arch${lo})"
    echo "$who does not select them: a lib/ or fixture"
    echo "helper is not a test; a *-selftest.sh belongs to scripts/run-selftests.sh;"
    echo "test-axl.sh is verify.sh's; and an arch= or local-only= gate can exclude"
    echo "one for this run only (an arch=none test runs in the x64 tree only)."
}

# The arch= values discovery understands. `none` is not a synonym for
# `both` by accident -- 25 host-only tests declare it, meaning "no arch of
# its own", and they run on every leg, which is what they have always done.
TEST_META_ARCHES="x64 aa64 both none"

# test_meta_arch_bad <script> — echo the arch= value if discovery does not
# understand it, nothing if it does.
#
# The `case` below had no default arm, so an unrecognised value fell through
# every branch and the test ran on BOTH arches. Three did: `arch=X64`,
# uppercase, in test-kbtune-qemu.sh, test-kbtune-driver-qemu.sh and
# test-tools-qemu.sh. A gate that cannot see a typo in its own metadata is
# the failure mode this whole file exists to close, so `--lint` fails on it
# and discover_tests says so loudly rather than guessing.
test_meta_arch_bad() {
    local a; a=$(test_meta_field "$1" arch)
    case " $TEST_META_ARCHES " in *" $a "*) return 0 ;; esac
    printf '%s\n' "$a"
}

# test_meta_cpu_bad <script> — echo the cpu= value if it is not a whole
# number from 1 to TEST_META_CPU_MAX; nothing when it is. The lint fails on
# it by name; the emitter registers such a test with 1 (test_meta_cpu).
test_meta_cpu_bad() {
    local c; c=$(test_meta_field "$1" cpu)
    # One digit, compared as a number: a long digit string would wrap in
    # bash arithmetic and pass a `<=` test.
    if [[ "$c" =~ ^[1-9]$ ]] && (( c <= TEST_META_CPU_MAX )); then return 0; fi
    printf '%s\n' "$c"
}

# test_meta_cpu <script> — the cpu= the registration uses: the declared value,
# or 1 when it is out of range (which the lint has already failed).
test_meta_cpu() {
    if [[ -n "$(test_meta_cpu_bad "$1")" ]]; then printf '1\n'; else test_meta_field "$1" cpu; fi
}

# test_meta_builds <script> — the declared `builds=` build arguments, one per
# line. Nothing when the test declares none.
test_meta_builds() {
    local v; v=$(test_meta_field "$1" builds)
    [[ -n "$v" ]] || return 0
    printf '%s\n' "$v" | tr ',' '\n' | grep -v '^$' || true
}

# test_make_args <script> — the build arguments the script ACTUALLY passes,
# one per line, read off its own `scripts/axl-build.sh ...` invocations (and
# those of any lib/*.sh it sources) -- the make command lines they replaced
# kept their shape, hence the name. Pre-built targets, the arch and the
# toolchain passthrough are dropped; everything else is what builds= must
# declare.
#
# A word this cannot resolve is printed as `?<word>` rather than dropped. An
# unreadable target and a test that builds nothing are the same empty list
# and opposite facts, and the lint has to be able to tell them apart.
# test_sourced_files <script> -- the script, then every lib/*.sh it sources,
# transitively, one path per line.
#
# What a test sources, it builds: three ssh tests reach their fixture
# only through lib/ssh-hostile-lib.sh, and four console-device tests only
# through lib/console-device-lib.sh. SOURCES -- a `source` or `.` line,
# not a comment -- and nothing else: a lib a test only names, or runs as
# the subject under test (test-ctest-integration.sh runs
# lib/suite-prep.sh), builds nothing on its behalf, and reading its make
# lines demanded builds= the test never performs.
#
# TRANSITIVELY: a lib a sourced lib sources builds for the test too. The
# split ssh idle suites are one-line wrappers over lib/ssh-idle-cases.sh,
# which reaches the fixture through lib/ssh-hostile-lib.sh; read one level
# deep, their builds= read as stale. Each file is read once, so a lib
# that sources itself, or two that source each other, still ends.
test_sourced_files() {
    local script="$1" dir files=() f i=0 lf
    dir="$(cd "$(dirname "$script")" && pwd)"
    files=("$script")
    while (( i < ${#files[@]} )); do
        while IFS= read -r f; do
            lf="$dir/lib/$f"
            [[ -f "$lf" ]] || continue
            [[ " ${files[*]} " == *" $lf "* ]] && continue
            files+=("$lf")
        done < <(grep -E '(^|[[:space:];&|{(])(source|\.)[[:space:]]' "${files[$i]}" 2>/dev/null \
                 | grep -vE '^[[:space:]]*#' \
                 | grep -oE 'lib/[A-Za-z0-9._-]+\.sh' | sed 's|^lib/||' | sort -u)
        i=$((i + 1))
    done
    printf '%s\n' "${files[@]}"
}

test_make_args() {
    local script="$1" files=() f joined line rest w vars tgts prebuilt
    mapfile -t files < <(test_sourced_files "$script")
    for f in "${files[@]}"; do
        # Backslash continuations are joined first, because most makes here
        # wrap across two lines and a line-at-a-time reader sees no targets
        # at all -- which reads as "clean". Comments go too: `make` in prose
        # is not a build, and a trailing one used to swallow the real
        # invocation whole.
        #
        # The `$(...)` collapse that used to sit HERE now runs per extracted
        # invocation, below. Collapsing the file first erased
        # `out=$(make -C … sneaky)` entirely -- an undeclared fixture the
        # gate then reported clean. Collapsing afterwards keeps what the
        # collapse is FOR (a tool built by PATH through a substitution that
        # contains spaces, which a whitespace tokenizer would tear in two)
        # and stops it hiding a make from the extractor.
        joined=$(sed -e 's/^[[:space:]]*#.*//' -e 's/[[:space:]]#[^"'"'"']*$//' "$f" \
                 | sed -e ':a' -e '/\\$/{N;s/\\\n//;ba}')
        while IFS= read -r line; do
            [[ -n "$line" ]] || continue
            # Collapse `$(...)` NOW, innermost first, so a substitution with
            # spaces in it is one token rather than two unreadable ones.
            rest=$(printf '%s\n' "$line" | sed -e ':s' -e 's/\$([^()]*)/@SUBST@/g' -e 'ts')
            # TWO INVOCATIONS THAT BUILD NOTHING the runner could pre-build,
            # skipped WHOLE and each for a reason, not because the parser
            # could not read them:
            #   -n           a dry run produces no file at all;
            #   PREFIX=      a DIFFERENT output tree from the one the runner
            #                pre-builds and tests stage from. Checked, not
            #                assumed: no recorded cache input lives under one,
            #                so nothing keys on those trees.
            case " $rest " in
                *" -n "*|*" PREFIX="*) continue ;;
            esac
            # BUILD=RELEASE is ALSO another tree (build/<arch>-release),
            # but one a test boots: test-efi-pack-release-qemu.sh stages the
            # release images from it, so they are cache inputs and must be
            # built before the cache decides, like any fixture. It used to be
            # skipped here with PREFIX=, which left such a test no way to
            # declare them. So it is read like any other invocation, except
            # that NOTHING is pre-built in that tree: `all` and `tools` there
            # are the test's own targets, and builds= must name them with
            # BUILD=RELEASE (the runner passes the variable through).
            prebuilt=" $TEST_PREBUILT_TARGETS "
            case " $rest " in *" BUILD=RELEASE "*) prebuilt=" " ;; esac
            vars=(); tgts=(); skipnext=0
            # Unquoted on purpose: this IS the tokenizer.
            # shellcheck disable=SC2086
            for w in $rest; do
                if [[ "$skipnext" -eq 1 ]]; then skipnext=0; continue; fi
                case "$w" in
                    # `-C DIR` -- the directory is the NEXT word, and every
                    # one in this tree is the project root under one name or
                    # another ($PROJECT_DIR, $DIR). Checked, not assumed; a
                    # make against some OTHER tree would need its own rule.
                    -C) skipnext=1; continue ;;
                    # A bare redirection digit left by the `2>&1` the
                    # extraction regex cut at, any other make FLAG, a
                    # `print-<var>` query target (which builds nothing).
                    # A `${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN}` passthrough used
                    # to be skipped here too; TOOLCHAIN means nothing to the
                    # Makefile and the CMake backend REFUSES it, so it is gone
                    # from every test and now fails this lint as unresolvable.
                    1|2) continue ;;
                    -*) continue ;;
                    print-*) continue ;;
                esac
                w="${w//\"/}"
                # A trailing `)` is the close of a command substitution the
                # collapse above could not reach, because its OPEN was the
                # thing that started this match: `out=$(make -C … foo)`.
                w="${w%)}"
                [[ -n "$w" ]] || continue
                if [[ "$w" == *'$'* || "$w" == *'@SUBST@'* ]]; then
                    # A file target under the pre-built tools/ tree is
                    # covered by `tools`; the arch and the toolchain
                    # passthrough are the runner's, not the test's.
                    [[ "$w" == */tools/*.efi ]] && continue
                    [[ "$w" == ARCH=* ]] && continue
                    printf '?%s\n' "$w"
                    continue
                fi
                if [[ "$w" == *=* ]]; then vars+=("$w"); else
                    case "$prebuilt" in *" $w "*) continue ;; esac
                    tgts+=("$w")
                fi
            done
            # A make that builds nothing the runner does not already build
            # needs no declaration -- INCLUDING its variables. Two dozen
            # tests spell `ARCH=x64 tools`, and requiring `builds=ARCH=x64`
            # from each of them would declare a build group with no targets
            # in it.
            [[ ${#tgts[@]} -gt 0 ]] || continue
            [[ ${#vars[@]} -gt 0 ]] && printf '%s\n' "${vars[@]}"
            printf '%s\n' "${tgts[@]}"
        # scripts/axl-build.sh, by any path that ends in it, ANYWHERE on the
        # line -- behind an flock prefix, in a command substitution -- read
        # as the make-shaped line it is; its own --print-prefix and --abs are
        # flags, skipped like any other, so a prefix query declares nothing.
        # A bare `make` of this tree is no longer a build at all: the lint
        # refuses one (test_calls_make).
        done < <(printf '%s\n' "$joined" \
                   | grep -oE 'scripts/axl-build\.sh"?[[:space:]]+[^|;&>]*' \
                   | sed -E 's/^scripts\/axl-build\.sh"?[[:space:]]+//' \
                   | sort -u)
    done
}

# test_calls_make <script> -- the lines that run `make` on this tree, in the
# script or a lib it sources (test_sourced_files), one per line (none:
# nothing printed). The Makefile was retired in 5.0.0, so such a line can only
# fail -- or, worse, build some OTHER make project in the test's cwd. Two
# shapes: `make -C "$PROJECT_DIR"` anywhere, and `make` (or gmake, by any
# path) at a command position, behind up to three prefix words (`nice -n 19
# make`, `timeout 300 make`, `env A=1 make`). Comments and continuations are
# dropped first. For the second shape, single-quoted strings are blanked
# (nothing runs in them) -- in one leftmost-first pass that skips a
# double-quoted string, so the apostrophes of "don't" and "it's" do not pair
# up and erase the code between them -- then each `$(...)` -- innermost first -- is replaced
# IN PLACE by a placeholder and its body put on a line of its own
# (`x="$(make foo)"` runs make), and then double-quoted strings are blanked
# ("built $(date); then make sure" is prose either side of the command).
# NOT seen, by design: `$MAKE`, a quoted command word (`"make"`) and a
# backtick substitution; still read as one: an unquoted `echo make ...`, a
# probe such as `command -v make`, and make in a heredoc body.
test_calls_make() {
    local f joined
    while IFS= read -r f; do
        joined=$(sed -e 's/^[[:space:]]*#.*//' -e 's/[[:space:]]#[^"'"'"']*$//' "$f" \
                 | sed -e ':a' -e '/\\$/{N;s/\\\n//;ba}')
        {
            grep -E 'make +-C +"?\$\{?PROJECT_DIR\}?"?' <<< "$joined"
            sed -E "s/(\"[^\"]*\")|'[^']*'/\1/g" <<< "$joined" \
                | awk '{ while (match($0, /\$\([^()]*\)/)) {
                             print substr($0, RSTART + 2, RLENGTH - 3)
                             $0 = substr($0, 1, RSTART - 1) "@S@" substr($0, RSTART + RLENGTH) }
                         print }' \
                | sed -E 's/"[^"]*"/""/g' \
                | grep -E '(^|\|\||&&|;|!|\(|(^|[[:space:]])(then|do|else))[[:space:]]*([-A-Za-z0-9_./="'"'"']+[[:space:]]+){0,3}([^[:space:]]*/)?g?make([[:space:];|&)]|$)'
        } | sort -u || true
    done < <(test_sourced_files "$1")
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
    # AXL_DISCOVER_DIR: discover another directory with THIS discover.sh --
    # how a stub directory is registered without a lib/ copy of its own.
    local dir; dir="${AXL_DISCOVER_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
    local f base arch lo
    for f in "$dir"/test-*.sh; do
        # A glob that matches nothing is passed through as itself: an empty
        # directory must discover nothing, not a test named `test-*.sh`.
        [[ -e "$f" ]] || continue
        base=$(basename "$f")
        # Skip the unit batch runner, the aggregate runner, and the self-tests.
        discover_is_test_name "$base" || continue
        arch=$(test_meta_field "$f" arch)
        lo=$(test_meta_field "$f" local-only)
        [[ "$lo" == "1" && $include_local -eq 0 ]] && continue
        [[ "$lo" != "1" && $only_local -eq 1 ]] && continue
        # VALIDITY IS ASKED OF ONE LIST, not spelled a second time here. A
        # `case` arm listing the good values beside TEST_META_ARCHES is two
        # copies of one fact, and they drift: a sabotage that added X64 to
        # the arm went UNDETECTED because the lint reads the other list.
        if [[ -n "$(test_meta_arch_bad "$f")" ]]; then
            # NOT a silent fall-through, and NOT a drop either: selection is
            # unchanged (the test is still returned, as it always was) but
            # the typo is announced, and `--lint` fails on it so it cannot
            # survive a commit.
            echo "discover: $base declares arch=$arch, which is none of" \
                 "$TEST_META_ARCHES -- selecting it on every arch" >&2
        else
            case "$arch" in
                x64)  [[ "$want_arch" == "X64" ]] || continue ;;
                aa64) [[ "$want_arch" == "AARCH64" ]] || continue ;;
                # both, none: every arch.
            esac
        fi
        printf '%s\n' "$f"
    done
}

# discover_emit_ctest ARCH RUN_ONE PREP -- print the CTest commands that
# register this directory's integration tests for ARCH (X64 | AARCH64), for a
# build tree's TEST_INCLUDE_FILES script to include at ctest time (slice 5a,
# docs/AXL-Build-System-Design.md §10.8). The SAME selection as the runner
# (discover_tests, local-only included -- CTest filters by label), one test
# per script, named integration.<script>, run through RUN_ONE
# (lib/run-one.sh); PREP (lib/suite-prep.sh) is registered once as the
# integration.prep fixture every test requires.
#
# arch=none tests are host-only and register in the X64 tree only, so a run of
# both trees runs each once. The header maps to:
#   LABELS            integration, arch:<value>, local-only, needs:<tool>...,
#                     guest:<class>
#   COST              est=, ONLY for a test not in AXL_IT_COSTED -- the tests
#                     the tree's CTestCostData.txt has measured, which the
#                     including script reads. ctest starts the costliest
#                     first, and for a test with a COST property it ignores
#                     its own measured average (and folds the COST into the
#                     average it writes back), so an unconditional est= would
#                     throw the measurement away on every run
#   PROCESSORS        cpu= (default 1): the slots of -j the test holds
#   WORKING_DIRECTORY ${AXL_IT_WORKDIR}: the source root, where the runner's
#                     tests run (ctest's default is the build tree's
#                     test/integration, and a test's `cmake --preset` found
#                     no presets there)
#   FIXTURES_REQUIRED integration.prep, and integration.tmp (the temp base's
#                     setup/cleanup pair, which the including script adds)
#   TIMEOUT           ${AXL_IT_TIMEOUT}, ${AXL_IT_TERM} (its SIGTERM and
#                     grace), ENVIRONMENT ${AXL_IT_ENV}, and the prep's
#                     ${AXL_IT_PREP_ENV}: set by the including script, at
#                     ctest time, so a timeout or cache choice needs no
#                     reconfigure
#   SKIP_RETURN_CODE  77
# Names and paths are CMake bracket arguments, so no character in them needs
# escaping.
# discover_tree_tests --arch X64|AARCH64 [discover_tests options] -- what a
# TREE registers: discover_tests, minus the arch=none (host-only) tests on
# AARCH64, which run once, in the x64 tree. ONE rule for the registration
# (--emit-ctest) and for run-integration.sh's selection, so the two cannot
# name different sets.
discover_tree_tests() {
    local want="X64" a f all
    local -a args=("$@")
    for ((a = 0; a < ${#args[@]}; a++)); do
        [[ "${args[a]}" == --arch ]] && want="${args[a + 1]:-X64}"
    done
    # Captured, not read through <(...): that would drop discover_tests' exit
    # status, and a discovery that could not run would read as an empty one.
    all=$(discover_tests "$@") || return $?
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        [[ "$want" == AARCH64 && "$(test_meta_field "$f" arch)" == none ]] && continue
        printf '%s\n' "$f"
    done <<< "$all"
}

# discover_drop_local_only PATH... -- the paths whose test-meta is NOT
# local-only=1, one per line (suite-prep.sh under AXL_PREP_SKIP_LOCAL=1).
discover_drop_local_only() {
    local f
    for f in "$@"; do
        [[ "$(test_meta_field "$f" local-only)" == 1 ]] || printf '%s\n' "$f"
    done
}

discover_emit_ctest() {
    local want="$1" run_one="$2" prep="$3" f b arch lo needs guest est cpu labels n
    local -a paths=() _needs=()
    local selected
    selected=$(discover_tree_tests --arch "$want" --include-local-only) || {
        echo "discover.sh --emit-ctest: discovery failed for $want" >&2; return 1; }
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        arch=$(test_meta_field "$f" arch)
        paths+=("$f")
        b=$(basename "$f" .sh)
        lo=$(test_meta_field "$f" local-only)
        needs=$(test_meta_field "$f" needs)
        guest=$(test_meta_pool_class "$f")
        est=$(test_meta_field "$f" est)
        [[ "$est" =~ ^[0-9]+$ ]] || est=20
        cpu=$(test_meta_cpu "$f")
        labels="integration;arch:$arch"
        [[ "$lo" == 1 ]] && labels+=";local-only"
        IFS=',' read -r -a _needs <<< "$needs"
        for n in ${_needs[@]+"${_needs[@]}"}; do [[ -n "$n" ]] && labels+=";needs:$n"; done
        [[ -n "$guest" ]] && labels+=";guest:$guest"
        # --retry: a failing test runs once more inside run-one.sh, which
        # keeps the first attempt's log. NOT ctest's --repeat, which repeats
        # every test -- the unit ratchet and gate.qloop included.
        printf 'add_test([==[integration.%s]==] bash [==[%s]==] --retry [==[%s]==])\n' "$b" "$run_one" "$f"
        # A guest= test is recorded with its class: pool mode (AXL_IT_POOL),
        # applied by the including script at ctest time, gives it a slot.
        [[ -n "$guest" ]] && printf 'list(APPEND AXL_IT_GUEST [==[integration.%s=%s]==])\n' "$b" "$guest"
        printf 'set_tests_properties([==[integration.%s]==] PROPERTIES LABELS [==[%s]==] PROCESSORS %s TIMEOUT ${AXL_IT_TIMEOUT} ${AXL_IT_TERM} SKIP_RETURN_CODE 77 FIXTURES_REQUIRED [==[integration.prep;integration.tmp]==] ENVIRONMENT "${AXL_IT_ENV}" WORKING_DIRECTORY "${AXL_IT_WORKDIR}")\n' \
            "$b" "$labels" "$cpu"
        printf 'if(NOT [==[integration.%s]==] IN_LIST AXL_IT_COSTED)\n    set_tests_properties([==[integration.%s]==] PROPERTIES COST %s)\nendif()\n' \
            "$b" "$b" "$est"
        local -a locks=()
        # A script that registers a stub directory (AXL_IT_DIR, or the est
        # gate's AXL_EST_IT_DIR) writes the tree's ONE stub registration
        # cache, integration-logs-stub; two of them side by side rewrite each
        # other's registration between its write and its include.
        if grep -qE '(^|[^A-Z_])AXL_(EST_)?IT_DIR=' "$f"; then
            locks+=(it-stub)
        fi
        if (( ${#locks[@]} > 0 )); then
            printf 'set_tests_properties([==[integration.%s]==] PROPERTIES RESOURCE_LOCK [==[%s]==])\n' \
                "$b" "$(IFS=';'; echo "${locks[*]}")"
        fi
    done <<< "$selected"
    printf 'add_test(integration.prep bash [==[%s]==]' "$prep"
    for f in ${paths[@]+"${paths[@]}"}; do printf ' [==[%s]==]' "$f"; done
    printf ')\n'
    printf 'set_tests_properties(integration.prep PROPERTIES LABELS prep FIXTURES_SETUP integration.prep TIMEOUT 3600 ENVIRONMENT "${AXL_IT_PREP_ENV}" WORKING_DIRECTORY "${AXL_IT_WORKDIR}")\n'
}

if [[ "${BASH_SOURCE[0]}" == "${0}" && "${1:-}" == "--emit-ctest" ]]; then
    [[ $# -eq 4 ]] || { echo "usage: discover.sh --emit-ctest X64|AARCH64 RUN_ONE PREP" >&2; exit 2; }
    discover_emit_ctest "$2" "$3" "$4"
    exit $?
fi

# `discover.sh --lint` — fail (and name offenders) if any QEMU integration test
# lacks a `# test-meta:` header, so a new test can't silently escape discovery /
# sharding. Run by gate.check-test-meta.
#
# It also holds `builds=` to the script's own build line, in BOTH directions.
# The declaration exists so the runner can build a test's fixtures before it
# decides the cache; a test that grows a second fixture and forgets the header
# puts itself straight back into the stale-key hole, silently, and a stale
# entry makes every run build something nothing needs. Two copies of one fact
# drift unless something compares them.
#
# TEST_LINT_DIR points it at a fixture directory (the self-test); unset, it
# lints the real one.
if [[ "${BASH_SOURCE[0]}" == "${0}" && "${1:-}" == "--lint" ]]; then
    # _discover_reaches_nsh_text FILE...: does running FILE[0], with the rest
    # sourced, call test_use_nsh_text? Each file's calls are named by the
    # TOP-LEVEL function they sit in ("-": the top level, which runs when
    # sourced); a function counts once some file calls it. Only column 0
    # opens or closes one -- the house style -- so a helper defined inside a
    # function (hostile_boot's own rand_hex) or an indented `}` cannot steal
    # or end the attribution; nor can a heredoc body (hostile_boot's
    # embedded askpass script), which is skipped to its terminator.
    _discover_reaches_nsh_text() {
        local fn
        while IFS= read -r fn; do
            [[ "$fn" == - ]] && return 0
            grep -qE "^[[:space:]]*${fn}([[:space:]]|\$)" "$@" && return 0
        done < <(awk '
            FNR == 1 { fn = ""; hd = "" }
            hd != "" {                 # inside a heredoc body: not shell code
                t = $0; if (strip) sub(/^\t+/, "", t)
                if (t == hd) hd = ""
                next }
            /^[A-Za-z_][A-Za-z0-9_]*\(\)[[:space:]]*\{/ {
                match($0, /[A-Za-z_][A-Za-z0-9_]*/); fn = substr($0, RSTART, RLENGTH) }
            /^\}/ { fn = "" }
            /^[[:space:]]*test_use_nsh_text[[:space:]]/ { print (fn == "" ? "-" : fn) }
            match($0, /<<-?[ \t]*["\047]?[A-Za-z_][A-Za-z0-9_]*/) {
                w = substr($0, RSTART + 2, RLENGTH - 2)
                strip = (substr(w, 1, 1) == "-"); sub(/^-/, "", w)
                gsub(/[ \t"\047]/, "", w); hd = w }' "$@")
        return 1
    }
    _d="${TEST_LINT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
    _classes="${TEST_LINT_CLASSES:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../pool" && pwd)/classes.toml}"
    _miss=0
    for _f in "$_d"/test-*.sh; do
        [[ -e "$_f" ]] || continue   # the unmatched glob itself (see discover_tests)
        _b="$(basename "$_f")"
        case "$_b" in
            test-axl.sh|test-all.sh|test-*-selftest.sh) continue ;;
        esac
        grep -q '^# test-meta:' "$_f" || { echo "missing test-meta: $_f"; _miss=1; }
        _cbad=$(test_meta_cpu_bad "$_f")
        [[ -z "$_cbad" ]] || {
            echo "$_b: cpu=$_cbad is not a whole number from 1 to $TEST_META_CPU_MAX"
            _miss=1
        }
        _bad=$(test_meta_arch_bad "$_f")
        [[ -z "$_bad" ]] || {
            echo "$_b: arch=$_bad is not one of $(printf '%s' "$TEST_META_ARCHES" | tr ' ' ',' | sed 's/,/, /g')."
            echo "    An unrecognised value is not filtered, so the test runs on EVERY arch."
            _miss=1
        }
        # Sorted -u on both sides: order and repetition carry no meaning
        # (three ssh tests name the same fixture twice, once themselves and
        # once through the lib they source).
        _declared=$(test_meta_builds "$_f" | sort -u)
        _actual=$(test_make_args "$_f" | sort -u)
        while IFS= read -r _w; do
            [[ -n "$_w" ]] || continue
            if [[ "$_w" == '?'* ]]; then
                echo "cannot tell what $_b builds: build argument ${_w#\?} does not resolve."
                echo "    Use a literal target name, or a path under the pre-built tools/ tree."
                _miss=1
                continue
            fi
            grep -qxF "$_w" <<< "$_declared" \
                || { echo "$_b builds $_w but does not declare it (add builds=... to its # test-meta: header)"; _miss=1; }
        done <<< "$_actual"
        while IFS= read -r _w; do
            [[ -n "$_w" ]] || continue
            grep -qxF "$_w" <<< "$_actual" \
                || { echo "$_b declares $_w but its build line does not build it (stale builds= entry)"; _miss=1; }
        done <<< "$_declared"
        # A size gate on a target the test does not build would walk the
        # graph of an image nobody measures -- and a typo'd target name would
        # route nothing at all. Held to builds=, which is held to the build
        # line above, so the chain ends at what the script really builds.
        while IFS= read -r _w; do
            [[ -n "$_w" ]] || continue
            grep -qxF "$_w" <<< "$_declared" \
                || { echo "$_b size-gates $_w, which its builds= does not build"; _miss=1; }
        done < <(test_meta_field "$_f" size-gate | tr ',' '\n')
        # The two halves of a size gate go together: a size-gate= with no
        # paths routes nothing to it, and paths with no target name nothing
        # the drift check can derive.
        if [[ -n "$(test_meta_field "$_f" size-gate)" && -z "$(size_gate_paths "$_f")" ]]; then
            echo "$_b size-gates $(test_meta_field "$_f" size-gate) but declares no size-gate-paths="; _miss=1
        fi
        if [[ -z "$(test_meta_field "$_f" size-gate)" && -n "$(size_gate_paths "$_f")" ]]; then
            echo "$_b declares size-gate-paths= but no size-gate= target"; _miss=1
        fi
        if [[ -z "$(test_meta_field "$_f" size-gate)" && -n "$(test_meta_field "$_f" size-gate-exclude)" ]]; then
            echo "$_b declares size-gate-exclude= but no size-gate= target"; _miss=1
        fi
        # build-backend= chose make or cmake for builds= until the Makefile
        # was retired (5.0.0): a test still declaring it names a build that
        # no longer exists.
        if [[ -n "$(test_meta_field "$_f" build-backend)" ]]; then
            echo "$_b: build-backend= is retired with the Makefile (5.0.0); remove it"
            _miss=1
        fi
        # ...and a test that still RUNS make on this tree builds nothing --
        # or some other make project in its cwd.
        _mk=$(test_calls_make "$_f")
        if [[ -n "$_mk" ]]; then
            echo "$_b runs make, which no longer builds this tree (5.0.0) -- build through scripts/axl-build.sh:"
            printf '%s\n' "$_mk" | sed 's/^/    /'
            _miss=1
        fi
        # guest=<class>: a pooled test runs on a class that exists, through one of
        # the three ways onto a lease: its checked-in .nsh via test_use_nsh, a
        # script it GENERATES with test_use_nsh_text, or scripts/run-qemu.sh
        # calls (its pooled mode, design §6.9) -- each way of getting that
        # wrong named here, not discovered at run time by a pooled guest
        # refusing it.
        _g=$(test_meta_field "$_f" guest)
        mapfile -t _srcs < <(test_sources "$_f" "$_d")
        # Every guest is accounted for: a test that launches one names the
        # class it runs on, or says `guest=none` -- the boot is deliberate --
        # with its reason beside it. Otherwise a test stays booted with nobody
        # having decided it should.
        if [[ -z "$_g" ]]; then
            if test_launches_guest "${_srcs[@]}"; then
                echo "$_b: launches a guest but declares no guest= (a pool class, or guest=none with a \"# guest=none: <why>\" comment)"
                _miss=1
            fi
        elif [[ "$_g" == none ]]; then
            [[ -n "$(test_meta_guest_none_reason "$_f")" ]] \
                || { echo "$_b: guest=none but gives no reason (add a \"# guest=none: <why>\" comment)"; _miss=1; }
            test_launches_guest "${_srcs[@]}" \
                || { echo "$_b: guest=none but launches no guest (drop it)"; _miss=1; }
        else
            if [[ ! -f "$_classes" ]]; then
                echo "$_b: guest=$_g, but there is no $_classes"; _miss=1
            elif ! grep -qE "^[[:space:]]*\[class\.${_g//./\\.}\][[:space:]]*(#.*)?$" "$_classes"; then
                echo "$_b: guest=$_g is not a class in $_classes"; _miss=1
            fi
            # A GENERATED script counts when the test REACHES test_use_nsh_text:
            # called at the top level of the test or of a lib/*.sh it sources
            # (transitively), or inside a function of one that the test or its
            # libs call -- lib/ssh-hostile-lib.sh's hostile_boot, for the twenty
            # tests that reach it, some through lib/ssh-idle-cases.sh. Merely
            # sourcing that lib for its helpers does not count.
            if grep -qE '^[[:space:]]*test_use_nsh([[:space:]]|$)' "$_f"; then
                [[ -f "${_f%.sh}.nsh" ]] \
                    || { echo "$_b: guest=$_g but no ${_b%.sh}.nsh beside it"; _miss=1; }
            elif _discover_reaches_nsh_text "${_srcs[@]}"; then
                :
            elif ! grep -qE '^[^#]*(scripts/run-qemu\.sh|\$\{?RUN_QEMU)' "$_f"; then
                echo "$_b: guest=$_g but runs no guest the pool can take (no test_use_nsh, no call reaching test_use_nsh_text, no scripts/run-qemu.sh call)"
                _miss=1
            fi
        fi
    done
    [[ $_miss -eq 0 ]] && echo "check-test-meta: clean ($(AXL_DISCOVER_DIR="$_d" discover_tests --include-local-only | wc -l) tests, builds= matches every build line)"
    exit $_miss
fi
