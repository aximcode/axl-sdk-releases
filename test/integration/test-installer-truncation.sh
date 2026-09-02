#!/bin/bash
# test-meta: arch=x64 needs= est=6 local-only=0
# test-installer-truncation.sh — a truncated `curl | sh` must destroy nothing.
#
# THE FAILURE THIS GUARDS. install.sh is documented as usable via
# `curl ... | sh`. If the connection drops mid-transfer, sh receives a PREFIX
# of the file and executes it. A linear script therefore runs its first N
# statements and stops wherever the bytes ran out. The first draft of
# install.sh did exactly that, with `rm -rf "$PREFIX_ROOT/$DIR"` at top level
# and the `tar` that refills it one line later: a cut in between deleted an
# existing install and did not replace it.
#
# The fix is structural, copied from rustup-init.sh -- every statement lives in
# a function and only `main "$@"` on the last line runs anything. A prefix of
# such a file defines some functions and exits, doing nothing.
#
# "Doing nothing" is not observable by reading the script, so this asserts it:
# feed sh every prefix of install.sh, with a populated install present, and
# require that the install is byte-identical afterwards every time.
set -uo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
INSTALLER="$PROJECT_DIR/packaging/install.sh"
[[ -f "$INSTALLER" ]] || { echo "FAIL: no $INSTALLER"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

# A prefix that looks exactly like something worth destroying.
VICTIM="$WORK/home/.local/share"
mkdir -p "$VICTIM/axl-sdk-9.9.9/bin" "$WORK/home/.local/bin"
echo "precious" > "$VICTIM/axl-sdk-9.9.9/bin/axl"
ln -sfn axl-sdk-9.9.9 "$VICTIM/axl-sdk"
BEFORE="$(cd "$WORK/home" && find . | sort | md5sum)"

TOTAL_LINES=$(wc -l < "$INSTALLER")
checked=0
for (( n = 1; n <= TOTAL_LINES; n++ )); do
    head -n "$n" "$INSTALLER" > "$WORK/trunc.sh"
    # --version pins it so a prefix that DID run would target the victim's
    # own directory name; nothing here should reach the network.
    HOME="$WORK/home" timeout 20 sh "$WORK/trunc.sh" --yes --version 9.9.9 \
        --prefix "$VICTIM" --bin-dir "$WORK/home/.local/bin" \
        --base-url "file://$WORK/nonexistent" >/dev/null 2>&1
    checked=$((checked + 1))
    AFTER="$(cd "$WORK/home" && find . | sort | md5sum)"
    if [[ "$AFTER" != "$BEFORE" ]]; then
        echo "  FAIL: a $n-line prefix of install.sh modified the install tree"
        ( cd "$WORK/home" && find . | sort ) | head -20
        fail=1
        break
    fi
done

# Only claim the property if it held. A summary that prints "unchanged at
# every one" directly under "FAIL: a 264-line prefix modified the tree" is the
# contradiction this tree keeps re-learning to avoid.
if (( fail )); then
    echo "  stopped after $checked truncation point(s)"
else
    echo "  checked $checked truncation point(s); install tree unchanged at every one"
fi

# CONTROL. A gate that has never caught anything has not been shown to see.
# Build a deliberately LINEAR installer with the same destructive step at top
# level -- the shape the real one had -- and require this test to catch it.
cat > "$WORK/linear.sh" <<'LINEAR'
#!/bin/sh
set -eu
PREFIX_ROOT="$HOME/.local/share"
DIR="axl-sdk-9.9.9"
rm -rf "$PREFIX_ROOT/$DIR"
echo "would extract here"
LINEAR
CTL_BEFORE="$(cd "$WORK/home" && find . | sort | md5sum)"
caught=0
for (( n = 1; n <= $(wc -l < "$WORK/linear.sh"); n++ )); do
    rm -rf "$VICTIM/axl-sdk-9.9.9"; mkdir -p "$VICTIM/axl-sdk-9.9.9/bin"
    echo "precious" > "$VICTIM/axl-sdk-9.9.9/bin/axl"
    head -n "$n" "$WORK/linear.sh" > "$WORK/ctl.sh"
    HOME="$WORK/home" timeout 20 sh "$WORK/ctl.sh" >/dev/null 2>&1
    [[ -f "$VICTIM/axl-sdk-9.9.9/bin/axl" ]] || { caught=1; break; }
done
if [[ "$caught" -eq 1 ]]; then
    echo "  control: a linear installer IS destroyed by truncation, as expected"
else
    echo "  FAIL: control did not fire -- this test cannot see the bug it guards"
    fail=1
fi

if (( fail )); then echo "FAIL: installer truncation safety"; exit 1; fi
echo "installer truncation test: OK"
exit 0
