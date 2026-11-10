#!/usr/bin/env python3
"""thread-exit-spinner.py -- a process with a thread that EXITS mid-run.

Two threads keep a CPU busy: a worker for WORKER_S seconds of wall time,
then it exits, and the main thread for MAIN_S seconds. Pinned to one CPU
(taskset), each waits for the other, so each accrues schedstat run delay --
and the worker takes its whole history with it when it exits, which is what
test-vcpu-starve-witness.sh needs to show the witness does not lose.

The spinning is hashlib over a 1 MiB buffer, which releases the GIL: two
Python threads spinning in bytecode would mostly SLEEP on the GIL instead,
and a sleeping thread waits for no CPU.

Prints "ready" (flushed) once the worker is running.

Usage: thread-exit-spinner.py WORKER_S MAIN_S
"""

from __future__ import annotations

import hashlib
import sys
import threading
import time

_BUF = bytes(1 << 20)


def spin(seconds: float) -> None:
    """Keep this thread runnable for `seconds` of wall time."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        hashlib.sha256(_BUF).digest()


def main(argv: list[str]) -> int:
    """Run the worker and the main spin; 2 on a usage error."""
    if len(argv) != 3:
        print("usage: thread-exit-spinner.py WORKER_S MAIN_S", file=sys.stderr)
        return 2
    worker_s = float(argv[1])
    main_s = float(argv[2])
    worker = threading.Thread(target=spin, args=(worker_s,))
    worker.start()
    print("ready", flush=True)
    spin(main_s)
    worker.join()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
