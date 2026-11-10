# shellcheck shell=bash
# wrap-rows-lib.sh -- what `stderr-row.efi wrap` writes and how a console of
# COLS columns shows it, for the suites that read it off a screen
# (test-stderr-row-qemu.sh, test-sshd-stderr-row-qemu.sh). Never run on its
# own.
#
# A UEFI console wraps the moment a character lands in its last column, so a
# line exactly COLS wide must be followed by the next line on the very next
# row -- not by an empty one, as its own CR LF used to leave -- and an empty
# line after one must still be exactly one empty row.

# wrap_text TAG LEN -- TAG then dots, LEN characters in all, as
# stderr-row.c's wrap_text.
wrap_text() {
    local t="$1"
    while (( ${#t} < $2 )); do t+=.; done
    printf '%s\n' "${t:0:$2}"
}

# wrap_lines -- the lines `stderr-row.efi wrap` writes at COLS columns, each
# after the stream it goes to: `o ` stdout, `e ` stderr (the log line and the
# leak verdict are standard error too); a log line's clock as `T`. The log
# line's prefix is stderr-row.c's SR_LOG_PREFIX_LEN, 36 characters.
wrap_lines() {
    local w=$COLS
    printf 'o '; wrap_text SR-W1 "$w"
    printf 'o '; wrap_text SR-W2 $((w - 1))
    printf 'o '; wrap_text SR-W3 $((w + 1))
    printf 'o '; wrap_text SR-W4 "$w"
    printf 'o '; wrap_text SR-W5 "$w"
    printf 'e '; wrap_text SR-W6 "$w"
    printf 'e T [ERROR] stderr-row: '; wrap_text SR-W7 $((w - 36))
    printf 'o '; wrap_text SR-W8 "$w"
    printf 'o '; wrap_text SR-W9 "$w"
    printf 'o \n'
    printf 'o SR-W-END\n'
    printf 'e T [INFO]  mem: no leaks detected\n'
}

# wrap_rows -- those lines as COLS-wide rows: each on the row after the one
# before it, the one past the width taking two.
wrap_rows() {
    local line
    wrap_lines | cut -c3- | while IFS= read -r line; do
        if (( ${#line} > COLS )); then
            printf '%s\n%s\n' "${line:0:COLS}" "${line:COLS}"
        else
            printf '%s\n' "$line"
        fi
    done
}
