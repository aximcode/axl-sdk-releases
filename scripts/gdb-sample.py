"""gdb-sample.py — periodic stack sampler driven from inside gdb.

Run under gdb attached to a free-running QEMU guest (the axl-sdk `--gdb`
stub). Every INTERVAL seconds it interrupts the guest, records the call
stack of the running thread, and resumes — the classic "poor man's
profiler" for a target where the host `perf` cannot see in. After SAMPLES
samples it writes two reports and quits gdb:

  <OUT>.folded   collapsed stacks (`a;b;c <count>`) for flamegraph.pl
  <OUT>.txt      a flat, self-contained profile (hottest leaf first)

Parameters arrive via environment variables (gdb's `-x` gives no argv):
  GDB_SAMPLE_SYMS      path to a gdb script that connects + loads symbols
                       (from scripts/gdb-syms.py); sourced once at start
  GDB_SAMPLE_COUNT     number of samples (default 200)
  GDB_SAMPLE_INTERVAL  seconds between samples (default 0.05)
  GDB_SAMPLE_OUT       output path stem (default /tmp/axl-profile)
  GDB_SAMPLE_DEPTH     max frames per stack (default 64)

Kept a separate file (not a bash heredoc) per the axl-sdk "no embedded
Python in bash scripts" rule; scripts/profile-qemu.sh invokes it with
`gdb -batch -x`.
"""

from __future__ import annotations

import os
import posixpath
import threading
import time

import gdb  # provided by the gdb runtime

# Live sample state, readable by the watchdog thread (which may have to write
# the report and force-exit if gdb wedges on an unsampleable guest).
FOLDED: dict[str, int] = {}
TAKEN = 0
DONE = False


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, "") or default)
    except ValueError:
        return default


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, "") or default)
    except ValueError:
        return default


COUNT = _env_int("GDB_SAMPLE_COUNT", 200)
INTERVAL = _env_float("GDB_SAMPLE_INTERVAL", 0.05)
DEPTH = _env_int("GDB_SAMPLE_DEPTH", 64)
OUT = os.environ.get("GDB_SAMPLE_OUT", "/tmp/axl-profile")
SYMS = os.environ.get("GDB_SAMPLE_SYMS", "")


def request_stop() -> None:
    """Fire an `interrupt` from a timer thread. Must hop to gdb's main thread
    via post_event — gdb's API is not thread-safe, and a synchronous
    `continue` on the main thread is what pumps the event loop that runs it."""
    def _do() -> None:
        try:
            gdb.execute("interrupt")
        except gdb.error:
            pass
    gdb.post_event(_do)


def frame_name(frame: "gdb.Frame") -> str:
    """A label for one frame: `function (file.c:line)` when the DWARF resolves,
    the bare function name if only the symbol is known, else the raw PC so an
    unsymbolized (firmware) frame is still attributable."""
    name = frame.name()
    if not name:
        try:
            block = frame.block()
            if block and block.function and block.function.name:
                name = block.function.name
        except (gdb.error, RuntimeError):
            name = None
    if not name:
        return f"0x{frame.pc():x}"
    # Append file:line from the symbol-and-line table when available — this is
    # the "where exactly" the profile is for.
    try:
        sal = frame.find_sal()
        if sal is not None and sal.symtab is not None and sal.line:
            return f"{name} ({posixpath.basename(sal.symtab.filename)}:{sal.line})"
    except (gdb.error, RuntimeError):
        pass
    return name


def capture_stack() -> list[str]:
    """Innermost-first list of frame labels for the running thread."""
    stack: list[str] = []
    try:
        frame = gdb.newest_frame()
    except gdb.error:
        return stack
    n = 0
    while frame is not None and n < DEPTH:
        stack.append(frame_name(frame))
        # Stop at the app entry point — unwinding past `main` walks into the
        # crt0 stub and then firmware, where the frame chain is unreliable and
        # produces junk labels (0x0, tiny addresses). The app call chain is
        # everything up to and including main.
        if frame.name() == "main":
            break
        try:
            frame = frame.older()
        except gdb.error:
            break
        n += 1
    return stack


def main() -> None:
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    # mi-async lets `interrupt` be posted and processed while a synchronous
    # `continue` pumps the event loop. Must be set before the target runs.
    gdb.execute("set mi-async on")
    # Connect + load symbols (the syms script ends with `target remote`).
    if SYMS:
        gdb.execute(f"source {SYMS}")

    global TAKEN, DONE
    aborted = 0

    # Never let a wedged gdb hang the tool: watch for a lack of PROGRESS (a
    # sample count that stops advancing) rather than an absolute wall-clock
    # budget — a slow-but-working TCG run must not be cut short and mislabeled.
    # Only a guest that genuinely can't be stopped (e.g. fully HALTED/idle,
    # never responding to `interrupt`) stalls the count; when that happens,
    # write what we have and hard-exit. Snapshot FOLDED to avoid racing the
    # main thread's dict updates.
    stall_after = max(15.0, INTERVAL * 40)   # seconds of no progress -> give up
    def watchdog() -> None:
        last = -1
        idle_since = time.monotonic()
        while not DONE:
            time.sleep(0.5)
            if TAKEN != last:
                last = TAKEN
                idle_since = time.monotonic()
            elif time.monotonic() - idle_since > stall_after:
                write_reports(dict(FOLDED), TAKEN,
                              note="the guest stopped yielding samples — it is "
                                   "most likely idle / HLT-bound (not spinning) "
                                   "or gdb could not stop it")
                os._exit(0)
    wd = threading.Thread(target=watchdog, daemon=True)
    wd.start()

    for _ in range(COUNT):
        # Arm a one-shot timer that will interrupt the guest one interval from
        # now, then `continue` (synchronous — it BLOCKS here and, crucially,
        # pumps gdb's event loop, which is what runs the posted interrupt and
        # delivers the remote stop). `continue` returns once the interrupt has
        # stopped the guest; then we sample its stack.
        timer = threading.Timer(INTERVAL, request_stop)
        timer.daemon = True
        timer.start()
        try:
            # to_string swallows the per-stop "Program received signal SIGINT"
            # + frame banner gdb prints on every interrupt.
            gdb.execute("continue", to_string=True)
        except gdb.error:
            aborted += 1
            timer.cancel()
            if aborted > 5:
                break
            continue
        timer.cancel()

        stack = capture_stack()
        if stack:
            key = ";".join(reversed(stack))   # outermost-first for flamegraph
            FOLDED[key] = FOLDED.get(key, 0) + 1
            TAKEN += 1

    DONE = True

    # Leave the guest running so the caller's QEMU teardown is unaffected.
    try:
        gdb.execute("continue &")
    except gdb.error:
        pass

    note = ""
    if aborted > 5:
        note = "sampling hit repeated gdb errors; results may be partial"
    write_reports(dict(FOLDED), TAKEN, note=note)
    gdb.execute("quit")


def write_reports(folded: dict[str, int], taken: int, note: str = "") -> None:
    # 1. Folded stacks (flamegraph input): "out;mid;leaf <count>".
    with open(f"{OUT}.folded", "w", encoding="utf-8") as f:
        for key, n in sorted(folded.items(), key=lambda kv: kv[1], reverse=True):
            f.write(f"{key} {n}\n")

    # 2. Flat profile by LEAF frame (where the CPU actually was), with a
    #    self+total split so a hot leaf and a hot caller both surface.
    leaf_self: dict[str, int] = {}
    func_total: dict[str, int] = {}
    for key, n in folded.items():
        frames = key.split(";")
        leaf = frames[-1]
        leaf_self[leaf] = leaf_self.get(leaf, 0) + n
        for fn in set(frames):     # total = stacks this fn appears in
            func_total[fn] = func_total.get(fn, 0) + n

    total = taken if taken else 1
    with open(f"{OUT}.txt", "w", encoding="utf-8") as f:
        f.write(f"axl-sdk QEMU profile — {taken} samples\n")
        if note:
            f.write(f"NOTE: {note}.\n")
        if taken == 0:
            f.write("\nNo stacks captured. The guest never stopped in "
                    "sampleable code — it is almost certainly idle/HLT-bound, "
                    "not burning CPU.\n")
            return
        f.write("\n")
        # If the hottest leaf is an unsymbolized raw address, the CPU is in
        # firmware (or an app built without symbols) — most often a WaitForEvent
        # HLT, i.e. the app is idle, not spinning. Say so up front.
        top_leaf = max(leaf_self.items(), key=lambda kv: kv[1])[0]
        if top_leaf.startswith("0x"):
            f.write("NOTE: the hottest frame is an unsymbolized address "
                    "(firmware, not app code) — the guest is most likely "
                    "idle / HLT-bound, or you need --ovmf-build-dir to name "
                    "firmware frames.\n\n")

        f.write("Hot leaves (self — where the CPU was executing):\n")
        f.write(f"{'self%':>7}  {'count':>6}  function\n")
        for fn, n in sorted(leaf_self.items(),
                            key=lambda kv: kv[1], reverse=True)[:25]:
            f.write(f"{100.0 * n / total:6.1f}%  {n:6d}  {fn}\n")
        f.write("\n")
        f.write("Hot stacks (total — including callees):\n")
        f.write(f"{'total%':>7}  {'count':>6}  function\n")
        for fn, n in sorted(func_total.items(),
                            key=lambda kv: kv[1], reverse=True)[:25]:
            f.write(f"{100.0 * n / total:6.1f}%  {n:6d}  {fn}\n")


main()
