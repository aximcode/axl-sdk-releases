# unit-times.awk -- each unit binary's own run time, from a ts-prefixed log.
#
# A unit binary prints a "=== <Name> Tests ===" header and a "=== Results:"
# footer; test_run_foreground stamps every line "[seconds] ". The time is
# header to footer, a footer pairing with the newest header that has none.
#
#   awk -f unit-times.awk LOG            one "NAME<TAB>SECONDS" line per binary,
#                                        in run order; SECONDS is "-" for a
#                                        binary that never printed its footer
#   awk -v total=1 -f unit-times.awk LOG the sum in milliseconds, or "-" when
#                                        nothing was measured (no stamps) or a
#                                        binary is incomplete -- never 0, which
#                                        a budget would read as "fast"
#
# One copy for both readers (common-test.sh: the per-binary timing table and
# the guest time a unit count file carries).
{
    # Strip ANSI escapes so the header and footer patterns match.
    gsub(/\x1b\[[0-9;]*[a-zA-Z]/, "")
}
/^\[[0-9.]+\].*=== .+ Tests ===/ {
    t = $1; gsub(/[\[\]]/, "", t)
    match($0, /=== .+ Tests ===/)
    name = substr($0, RSTART + 4, RLENGTH - 14)
    n++
    names[n] = name
    begin_t[n] = t + 0.0
}
/^\[[0-9.]+\].*=== Results:/ {
    t = $1; gsub(/[\[\]]/, "", t)
    for (i = n; i >= 1; i--) {
        if (!(i in end_t)) {
            end_t[i] = t + 0.0
            break
        }
    }
}
END {
    if (total) {
        if (n == 0) { print "-"; exit }
        for (i = 1; i <= n; i++) {
            if (!(i in end_t)) { print "-"; exit }
            sum += end_t[i] - begin_t[i]
        }
        printf "%d\n", sum * 1000 + 0.5
        exit
    }
    for (i = 1; i <= n; i++) {
        if (i in end_t) {
            printf "%s\t%.3f\n", names[i], end_t[i] - begin_t[i]
        } else {
            printf "%s\t-\n", names[i]
        }
    }
}
