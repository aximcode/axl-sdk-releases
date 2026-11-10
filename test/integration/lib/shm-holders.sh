#!/bin/bash
# lib/shm-holders.sh -- which processes still hold a file under a directory
# open (an fd) or mapped (a mapping). Sourced by the harness self-tests.
#
# WHY NOT `df`. A file deleted while a process holds it keeps its tmpfs pages
# until the last fd or mapping goes: the directory is gone, `du` reads zero,
# and only a `df -k /dev/shm` delta used to see the leak. But df is GLOBAL --
# any other run writing /dev/shm moves it -- so a self-test that asserted on
# it could only run alone, and the self-tests ran serially for that reason.
# The pages are held by exactly the processes this lists, so "nothing listed"
# is the leak check itself, scoped to one base: what df stood in for, with
# nobody else's churn in the number.
#
# WHAT IT CANNOT SEE: another user's processes (their /proc/PID/fd is not
# readable), and a holder in another MOUNT NAMESPACE (a sandboxed virtiofsd
# reports paths from its own root). Every holder these tests create is our
# own and in our namespace. A base reached through a symlink is resolved
# first (the kernel shows resolved paths), and a base containing a glob
# character is refused (-lname would read it as a pattern). The scan's exit
# status cannot say "could not look" -- find reports EACCES for other users'
# processes on every run -- so each caller takes a POSITIVE CONTROL instead:
# it must list a holder it planted before it may believe an empty answer.

# shm_holders <absolute-dir> -- print "PID fd|map TARGET" for each open fd or
# mapping whose target lies under the directory (a deleted target reads
# "PATH (deleted)" and is listed). Prints nothing when there are none.
# Returns 2 on a relative or empty directory ("/" would match everything and
# a relative path nothing, and either would read as an answer) or a glob one.
shm_holders() {
    local base="${1%/}" m
    if [[ -z "$base" || "$base" != /* ]]; then
        echo "shm_holders: need an absolute directory, got '${1:-}'" >&2
        return 2
    fi
    if [[ "$base" == *[\[\]\*\?]* ]]; then
        echo "shm_holders: '$base' contains a glob character -- refused, not guessed" >&2
        return 2
    fi
    # -m: resolves the symlinks that exist without requiring the base itself
    # to (a caller may ask after removing it -- that is the deleted-inode check).
    base=$(readlink -m -- "$base")
    # -lname matches the LINK TEXT, which is what the kernel shows for an fd.
    find /proc/[0-9]*/fd -maxdepth 1 -type l -lname "$base/*" \
        -printf '%h %l\n' 2>/dev/null \
        | sed -E 's|^/proc/([0-9]+)/fd |\1 fd |'
    for m in $(grep -lF -- " $base/" /proc/[0-9]*/maps 2>/dev/null); do
        m=${m#/proc/}
        printf '%s map %s\n' "${m%/maps}" "$base"
    done
    return 0
}
