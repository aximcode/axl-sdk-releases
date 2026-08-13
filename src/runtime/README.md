AXL Runtime — lifecycle services for an AXL app
================================================

The pieces an AXL app interacts with around its own lifecycle and
interruptibility. The CRT0 entry stub
(`src/crt0/axl-crt0-native.c`, ~17 lines) bridges the UEFI entry
point to `int main(int argc, char **argv)` by calling
`_axl_init` before `main` and `_axl_cleanup` after. **This
module — the AXL runtime — implements those two bookends and
everything they wire up**: the default event loop, the cooperative
yield, the interrupt handler registry, the tier-1 resource-leak
sweep, and the LIFO atexit callback registry. CRT0 holds none of
that state itself; it just calls in and back out.

The full lifecycle (init → main → cleanup → exit) and the
runtime-vs-CRT0 split are documented in
[`docs/AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md). This README is the
module-level reference for the runtime sources.

Four sub-modules, each a single concern:

- ``axl-runtime.c`` — `_axl_init` / `_axl_cleanup`, the
  singleton `axl_loop_default()`, and `axl_yield`.
- ``axl-registry.c`` — tier-1 firmware-resource registry.
  Internal-only API (`_axl_registry_*`) called by the `_new_impl` /
  `_free` paths of AxlEvent, AxlLoop, AxlCancellable, and AxlArena.
  Sweeps leaked resources during `_axl_cleanup`.
- ``axl-atexit.c`` — POSIX-flavored cleanup registry
  (`axl_atexit` / `axl_atexit_remove`). LIFO drain during
  `_axl_cleanup`, before the tier-1 sweep.
- ``axl-signal.c`` — `axl_signal_install` / `axl_interrupted` /
  `axl_exit`. Hooked into loop break detection; invokes the user
  handler once per Ctrl-C and sets the interrupted flag.

Headers:

- ``<axl/axl-runtime.h>`` — default loop, yield, registry count
- ``<axl/axl-signal.h>`` — interrupt API + blessed exit
- ``<axl/axl-atexit.h>`` — LIFO cleanup callbacks
- ``<axl/axl-loop.h>`` — includes ``axl_loop_iterate_until``
  (nested-wait primitive), the loop-module partner for callers
  inside a callback that need to wait without freezing outer
  sources

## When to Use What

| I need to... | Use |
|---|---|
| Handle Ctrl-C with a custom callback | `axl_signal_install(fn)` |
| Ask "did Ctrl-C happen yet?" from a CPU loop | `axl_interrupted()` |
| Exit with cleanup guaranteed (vs raw `gBS->Exit`) | `axl_exit(rc)` |
| Keep a tight CPU loop responsive to Ctrl-C | `axl_yield()` every N iters |
| Free a long-lived resource on any exit path | `axl_atexit(fn, data)` |
| Share a loop across modules | `axl_loop_default()` |
| Wait inside a callback without starving the outer loop | `axl_loop_iterate_until(loop, done, timeout_us)` |
| Serialize a short critical section against a pump callback | `axl_tpl_raise(AXL_TPL_NOTIFY)` / `axl_tpl_restore(prev)` |
| Ask what priority level I am running at | `axl_tpl_current()` |
| Contain a suspect region that may leave the level raised | `axl_tpl_restore_baseline(&leaked)` |

**Library re-entrancy guarantee.** `axl_yield()`, when a default loop exists,
runs one non-blocking dispatch of it (the yield-as-scheduler idiom) — so it can
fire *your* idle/timer/event callbacks. That is only ever invoked when *your*
code calls `axl_yield()` directly. AXL's own long-running operations
(`axl_qsort`, `axl_digest`, large `axl_fs` copies, the sync HTTP client, IPMI
KCS polling) stay Ctrl-C responsive via an internal break-only poll
(`_axl_poll_break`) that observes the interrupt **without** dispatching the
loop — so a library call can never re-enter your callbacks from deep inside an
unrelated operation.

## Interrupt lifecycle

```
user presses Ctrl-C
   |
   v
shell signals its ExecutionBreak event
   |
   +--- loop observes via axl_backend_shell_break_flag /
   |    axl_backend_shell_break_event (in axl_loop_next_event)
   |
   +--- OR axl_yield observes via a non-blocking loop dispatch
   |    (or a direct poll when no default loop exists)
   |
   v
_axl_signal_on_break() fires once:
   - sets g_axl_interrupted = true
   - if axl_signal_install(fn) was called, invokes fn()
   - idempotent (subsequent observations no-op)
   |
   v
yield / loop_run / wait_* return with status indicating interrupt
   |
   +--- if user handler installed: caller unwinds main, CRT0
   |    runs _axl_cleanup on main's return
   |
   +--- if no handler: next axl_yield() auto-calls axl_exit(1),
        which takes the same _axl_cleanup path + gBS->Exit
```

The two exit paths (`return` from main, `axl_exit(rc)`) both
converge on `_axl_cleanup`, so cleanup output is byte-identical
between them. `_axl_cleanup` has a reentrancy guard: if
`axl_exit` fires mid-main and the firmware somehow returns from
`gBS->Exit` (paranoia), CRT0's post-main cleanup is a no-op.

## Cleanup order

`_axl_cleanup` runs these in order:

0. **TPL repair** — `axl_tpl_restore_baseline`. An image that returns
   above `TPL_APPLICATION` wedges the machine: measured at
   `TPL_CALLBACK` as well as `TPL_NOTIFY`, so *every* raised level is
   fatal, and on x64 a release firmware says nothing and simply spins
   (AArch64 at least asserts `Image->Tpl == gEfiCurrentTpl` first).
   This runs before everything below it because at `TPL_HIGH_LEVEL`
   the reporting would itself hang — `AllocatePool`, `CreateEvent`,
   `SetTimer`, `CloseEvent` and console output all block rather than
   failing. So the level is repaired first and the defect logged
   after, which turns an unrecoverable hang into a named error.
1. **atexit callbacks** (LIFO) — `_axl_atexit_run_all`. User
   callbacks may free resources that would otherwise show up in
   the sweep.
2. **argv strings** — `_axl_args_free` in `src/posix/axl-app.c`.
3. **Default loop** — explicit `axl_loop_free(mDefaultLoop)` so
   its registry entry comes off cleanly (otherwise sweep would
   flag it as a leak on every exit).
4. **Tier-1 registry sweep** — `_axl_registry_sweep`. LIFO walk
   of live entries, each logged with user `file:line` and closed
   via the appropriate `_free`.
5. **Heap leak report** (AXL_MEM_DEBUG only) --
   `_axl_mem_dump_leaks_at_exit`, the teardown-flavoured
   `axl_mem_dump_leaks` (its header omits the diagnostic form's
   `(live allocations)` infix, which is how the QEMU harness tells a
   verdict from a running program's dump). Runs LAST on purpose:
   everything above it is a reclaim step, so anything still live here
   has no owner left. Release-mode auto-free of heap is
   deferred (see [`docs/AXL-Lifecycle.md` §10.1](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#101-release-mode-heap-auto-sweep)).

## Caller attribution

Every tier-1 resource allocation records the caller's file/line at
macro-expansion time:

```c
AxlEvent *e = axl_event_new();   // expands to:
                                 //   axl_event_new_impl(__FILE__, __LINE__)
```

When the sweep fires on a leak, the warning names that file/line
-- the app developer's call site, not the library's internal
wrapper. Library-internal callers (e.g., `axl_cancellable_new`
internally calling `axl_event_new`) record the library's own file/
line by design; library code that correctly frees never reaches
the sweep, so the only way those appear is if the library itself
leaks — in which case the library source is the correct
attribution.

## See also

- [`docs/AXL-Lifecycle.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md) — the design
  doc, now describing what landed.
- [`docs/AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md) --
  primitive taxonomy; the runtime sits under these primitives.
- [`sdk/examples/runtime-demo.c`](https://github.com/aximcode/axl-sdk-releases/blob/main/sdk/examples/runtime-demo.c)
  — eight subcommand scenarios exercising every facet.
- [`src/loop/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/loop/README.md) — `AxlLoop`,
  `AxlDefer`, `AxlPubsub`, and the nested-wait primitive.
