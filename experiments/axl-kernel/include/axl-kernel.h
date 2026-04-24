/**
 * axl-kernel.h — POC public API (K1 + K2 + K3 partial).
 *
 * K1 + K2: process control (spawn/exit/wait/yield/sleep, pids).
 * K3:      fd table + TCP syscalls (listen/accept/read/write/close).
 *
 * If you need more, you are out of POC scope (§13 of
 * AXL-Kernel-Design.md).
 *
 * Usage:
 *   int pid1(int argc, char **argv) { ...; return 0; }
 *   int main(int argc, char **argv) {
 *       if (axlk_init() != 0) return 1;
 *       return axlk_run(pid1, argc, argv);
 *   }
 */

#ifndef AXL_KERNEL_H
#define AXL_KERNEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t AxlkPid;
#define AXLK_PID_ANY ((AxlkPid)-1)

/** @brief Flag for axlk_waitpid: return 0 immediately if no child is
 *  ready, instead of blocking. Matches POSIX WNOHANG semantics. */
#define AXLK_WNOHANG 1

typedef int (*AxlkProcMain)(int argc, char **argv);

/**
 * @brief Initialize the kernel. Call once before axlk_run.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int
axlk_init(void);

/**
 * @brief Start the scheduler with @p entry as pid 1.
 *
 * Blocks until pid 1 exits. Returns its exit status.
 */
int
axlk_run(
    AxlkProcMain entry,   ///< pid 1 entry function
    int         argc,    ///< argv count passed to pid 1
    char      **argv     ///< argv passed to pid 1
);

/**
 * @brief Spawn a new cooperative process.
 *
 * Parent is the current process. @p stack_kib = 0 uses the POC
 * default (16 KiB). Over-the-default requests are rejected in POC;
 * resizing arrives post-K2.
 *
 * @return new pid on success, -1 on failure.
 */
AxlkPid
axlk_spawn(
    AxlkProcMain entry,
    int         argc,
    char      **argv,
    size_t      stack_kib
);

/**
 * @brief Terminate the current process with @p status. Does not return.
 */
void axlk_exit(int status) __attribute__((noreturn));

/**
 * @brief Block until a child exits.
 *
 * @p pid = AXLK_PID_ANY waits for any child. @p status (if non-NULL)
 * receives the exit status.
 *
 * @return the pid that exited, or -1 if no children exist.
 *
 * Thin wrapper around axlk_waitpid(pid, status, 0).
 */
AxlkPid
axlk_wait(
    AxlkPid  pid,
    int    *status
);

/**
 * @brief POSIX-shaped waitpid. Reaps a child; optionally non-blocking.
 *
 * @p pid = AXLK_PID_ANY waits for any child. @p status (if non-NULL)
 * receives the exit status on a successful reap.
 *
 * Without AXLK_WNOHANG: blocks until a matching child exits, exactly
 * like axlk_wait.
 *
 * With AXLK_WNOHANG: returns 0 immediately if a matching child exists
 * but hasn't exited yet. This is the pattern services want for
 * draining zombies between accepts without blocking the accept loop.
 *
 * @return > 0 reaped pid, 0 if WNOHANG and no child ready,
 *         -1 if no matching child exists at all.
 */
AxlkPid
axlk_waitpid(
    AxlkPid  pid,
    int    *status,
    int      flags    ///< 0 or AXLK_WNOHANG
);

/** @brief Yield the CPU back to the scheduler. */
void axlk_yield(void);

/** @brief Cooperative sleep. Yields until @p ms milliseconds have elapsed. */
void axlk_sleep_ms(uint32_t ms);

/** @brief Current process's pid. */
AxlkPid axlk_getpid(void);

/** @brief Parent pid of the current process. */
AxlkPid axlk_getppid(void);

/** @brief Number of live processes (ready + waiting, excluding zombies). */
size_t axlk_proc_count(void);

// ---------------------------------------------------------------------------
// K3 — fd table + TCP syscalls
//
// fds are small positive ints (starting at 1; 0 / negative = error).
// The POC uses a kernel-global fd table: any process that knows an fd
// number can use it. Proper per-process tables arrive post-K3.
//
// Each blocking syscall yields to the scheduler while the underlying
// axl-sdk async op runs; other processes make progress meanwhile.
// Ctrl-C during a wait returns -1 from the syscall.
// ---------------------------------------------------------------------------

/**
 * @brief Create a TCP listener on @p port.
 *
 * Returns immediately — no yield. The returned fd is passed to
 * axlk_accept to wait for connections.
 *
 * @return listener fd on success, -1 on failure.
 */
int axlk_listen(uint16_t port);

/**
 * @brief Accept one connection on a listener fd. Yields until a client connects.
 *
 * @return client fd on success, -1 on failure or Ctrl-C.
 */
int axlk_accept(int listen_fd);

/**
 * @brief Read up to @p n bytes from a connected TCP fd. Yields until data.
 *
 * @return bytes received (>0), 0 on peer close, -1 on error.
 */
int axlk_read(int fd, void *buf, size_t n);

/**
 * @brief Write @p n bytes to a connected TCP fd. Yields until drained.
 *
 * @return 0 on success, -1 on failure.
 */
int axlk_write(int fd, const void *buf, size_t n);

/**
 * @brief Close an fd and release its underlying resource. Invalid-fd-safe.
 */
void axlk_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* AXL_KERNEL_H */
