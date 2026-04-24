/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-signal.h:
 *
 * POSIX-flavored interrupt handler API. The axl_signal_* namespace
 * was freed up pre-1.0 (see the axl_pubsub_* rename) specifically
 * to host this surface.
 *
 * At runtime:
 *   - CRT0 (via _axl_init) installs a notify path on the shell
 *     ExecutionBreak event. When the user presses Ctrl-C, the
 *     runtime sets a global "interrupted" flag and, if the app
 *     has installed a handler via axl_signal_install, invokes it.
 *   - The handler runs in a limited context (raised TPL). It is
 *     expected to set an app flag, log, and return — not to
 *     allocate, block, or call Boot Services that mutate state.
 *     Cleanup happens at the next yield point or inside the app's
 *     own axl_atexit callback chain.
 *   - If no handler is installed, the default behavior is to
 *     terminate the app cleanly at the next axl_yield observation
 *     (runs _axl_cleanup, then gBS->Exit via axl_backend_boot_exit).
 *
 * @code
 * static volatile bool g_should_quit;
 *
 * static void on_interrupt(void) {
 *     g_should_quit = true;
 * }
 *
 * int main(int argc, char **argv) {
 *     axl_signal_install(on_interrupt);
 *     while (!g_should_quit && more_work()) {
 *         do_work();
 *         axl_yield();
 *     }
 *     return g_should_quit ? 1 : 0;
 * }
 * @endcode
 *
 * See docs/AXL-Runtime.md §2.2 and §4.4 for the design.
 */

#ifndef AXL_SIGNAL_H
#define AXL_SIGNAL_H

#include <stdbool.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AxlSignalHandler:
 *
 * Interrupt-time callback. Runs at raised TPL when the shell
 * ExecutionBreak event fires. Do not allocate, do not block, do not
 * call Boot Services that mutate state. Set a flag, log, return.
 */
typedef void (*AxlSignalHandler)(void);

/**
 * @brief Install a handler fired on Ctrl-C. Overrides the default
 *     exit-on-interrupt behavior.
 *
 * Passing NULL is equivalent to axl_signal_default(): no user handler,
 * the runtime will exit cleanly at the next yield point.
 */
void
axl_signal_install(
    AxlSignalHandler on_interrupt
);

/**
 * @brief Restore the default handler (auto-exit on next yield).
 *
 * Equivalent to axl_signal_install(NULL) — named for readability.
 */
void
axl_signal_default(void);

/**
 * @brief Poll the interrupted flag.
 *
 * True between the break event firing and _axl_cleanup clearing it
 * (which only happens as part of axl_exit). App code reads this to
 * decide whether to keep working or unwind.
 */
AXL_WARN_UNUSED bool
axl_interrupted(void);

/**
 * @brief Terminate the app with guaranteed cleanup. Does not return.
 *
 * Runs atexit callbacks (LIFO), sweeps the tier-1 resource registry,
 * then calls gBS->Exit via the backend. This is the ONE blessed exit
 * path — returning from main takes the same route via CRT0. App
 * code that calls gBS->Exit directly, or aborts through some other
 * path, bypasses cleanup and leaks firmware resources; don't.
 *
 * Convention: rc == 0 -> EFI_SUCCESS, any other value -> EFI_ABORTED.
 */
AXL_NORETURN void
axl_exit(int rc);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SIGNAL_H */
