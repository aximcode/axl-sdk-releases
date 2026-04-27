/**
 * kernel.c — axl-kernel POC (K1 + K2 + K3).
 *
 * Cooperative coroutine scheduler with fixed-slot PCB table, stack
 * pool, kernel-global fd table, and TCP syscalls backed by
 * axl-sdk's async TCP API. "PCB" is the conventional OS term for
 * Process Control Block — the per-process struct that holds pid,
 * state, saved registers, stack pointer, parent/child links, exit
 * status (Linux calls it `task_struct`; FreeBSD `struct proc`).
 * See ../AXL-Kernel-Design.md §13 for the spec.
 *
 * AXL API use policy: cold paths use axl-sdk freely (axl_printf,
 * axl_time_get_ms, axl_strcmp, axl_tcp_*). The hot paths — ready
 * queue, sleep list, context switch — deliberately use intrusive
 * linked lists embedded in AxlkProc rather than AxlQueue / AxlList,
 * because the container APIs allocate a node per enqueue via
 * axl_malloc. A scheduler can't afford heap churn on every yield.
 * Same reason Linux uses `list_head` and FreeBSD uses `TAILQ`.
 *
 * Scheduler + AxlLoop: the kernel owns an AxlLoop (`sched_loop`)
 * created at axlk_init. Blocking syscalls register an async op on
 * this loop and suspend their PCB; the completion callback marks
 * the PCB ready. The scheduler's idle path is
 * `axl_loop_next_event(sched_loop, true)` — one real
 * gBS->WaitForEvent call that hands CPU back to the hypervisor
 * until any completion fires. No busy-spin.
 *
 * Scheduler stack: the main() thread's stack is reused as the
 * scheduler stack. When axlk_run is called, we save the caller's
 * context as `sched_ctx` and every user-proc exit / yield switches
 * back to it. When no processes remain, axlk_run returns.
 */

#include <axl.h>
#include "axl-kernel.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

#define AXLK_POC_MAX_PROCS   16
#define AXLK_POC_STACK_KIB   16
#define AXLK_POC_STACK_BYTES (AXLK_POC_STACK_KIB * 1024)

#define AXLK_STACK_CANARY    UINT64_C(0xDEADBEEFCAFEBABE)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef enum {
    AXLK_PROC_FREE = 0,   /* slot unused */
    AXLK_PROC_READY,
    AXLK_PROC_RUNNING,
    AXLK_PROC_WAITING,
    AXLK_PROC_ZOMBIE,
} AxlkProcState;

/* AxlkCtx layout is arch-specific. Must match the corresponding
 * ctx-switch-*.S file. */
#if defined(__x86_64__)
typedef struct AxlkCtx {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
} AxlkCtx;
#elif defined(__aarch64__)
/* AAPCS64 callee-saved: x19-x28, x29 (FP), x30 (LR), sp.
 * FP/SIMD (v8-v15 lower 64b) not saved — freestanding code
 * doesn't emit SIMD in the scheduler path; extend if that changes. */
typedef struct AxlkCtx {
    uint64_t x19, x20;
    uint64_t x21, x22;
    uint64_t x23, x24;
    uint64_t x25, x26;
    uint64_t x27, x28;
    uint64_t x29;    /* FP */
    uint64_t x30;    /* LR */
    uint64_t sp;
} AxlkCtx;
#else
#  error "axl-kernel: unsupported architecture"
#endif

typedef struct AxlkProc {
    AxlkPid           pid;
    AxlkPid           ppid;
    AxlkProcState     state;
    AxlkCtx           ctx;

    uint8_t         *stack_base;   /* lowest address; canary lives here */
    size_t           stack_size;
    int              stack_slot;   /* -1 if not from pool */

    AxlkProcMain      entry;
    int              argc;
    char           **argv;
    int              exit_status;

    /* Wait / sleep */
    uint64_t         wake_at_ms;    /* 0 = not sleep-waiting */
    AxlkPid           wait_for_pid;  /* 0 = not child-waiting */

    /* Syscall return channel — written by completion callback on
     * the scheduler stack, read by the syscall wrapper when the
     * suspended proc resumes. Only one blocking syscall in flight
     * per proc, so one slot is enough. */
    intptr_t         syscall_result;

    struct AxlkProc  *next_ready;    /* ready/zombie FIFO link */
    struct AxlkProc  *next_sleep;    /* sleep list link */
} AxlkProc;

// ---------------------------------------------------------------------------
// fd table — kernel-global. Per-process tables arrive post-POC.
// ---------------------------------------------------------------------------

#define AXLK_MAX_FDS 32

typedef enum {
    AXLK_FD_FREE = 0,
    AXLK_FD_TCP_LISTENER,
    AXLK_FD_TCP_CONN,
} AxlkFdKind;

typedef struct {
    AxlkFdKind  kind;
    AxlTcp     *tcp;     /* used by both TCP_LISTENER and TCP_CONN */
} AxlkFdSlot;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

extern void axlk_ctx_switch(AxlkCtx *from, AxlkCtx *to);

static AxlkProc  pcb_table[AXLK_POC_MAX_PROCS];
static uint8_t  stack_pool[AXLK_POC_MAX_PROCS][AXLK_POC_STACK_BYTES]
                __attribute__((aligned(16)));
static bool     stack_used[AXLK_POC_MAX_PROCS];

static AxlkProc *current_proc = NULL;
static AxlkProc *ready_head = NULL, *ready_tail = NULL;
static AxlkProc *zombie_head = NULL;
static AxlkProc *sleep_head = NULL;

static AxlkCtx   sched_ctx;          /* saved when user code calls axlk_run */
static AxlkPid   next_pid_val = 1;
static int      pid1_exit_status = 0;
static bool     kernel_initialized = false;

static AxlLoop    *sched_loop = NULL;        /* kernel-owned event loop */
static AxlkFdSlot  fd_table[AXLK_MAX_FDS];    /* index 0 reserved for sentinel */

// ---------------------------------------------------------------------------
// PCB + stack pool
// ---------------------------------------------------------------------------

static AxlkProc *
pcb_alloc(void)
{
    for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
        if (pcb_table[i].state == AXLK_PROC_FREE) {
            AxlkProc *p = &pcb_table[i];
            p->pid          = next_pid_val++;
            p->ppid         = (current_proc != NULL) ? current_proc->pid : 0;
            p->state        = AXLK_PROC_READY;
            p->stack_base   = NULL;
            p->stack_size   = 0;
            p->stack_slot   = -1;
            p->entry        = NULL;
            p->argc         = 0;
            p->argv         = NULL;
            p->exit_status  = 0;
            p->wake_at_ms   = 0;
            p->wait_for_pid = 0;
            p->next_ready   = NULL;
            p->next_sleep   = NULL;
            return p;
        }
    }
    return NULL;
}

static void
pcb_free(AxlkProc *p)
{
    p->state      = AXLK_PROC_FREE;
    p->pid        = 0;
    p->stack_base = NULL;
    p->stack_slot = -1;
}

static int
stack_pool_alloc(void)
{
    for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
        if (!stack_used[i]) {
            stack_used[i] = true;
            return i;
        }
    }
    return -1;
}

static void
stack_pool_free(int slot)
{
    if (slot >= 0 && slot < AXLK_POC_MAX_PROCS) {
        stack_used[slot] = false;
    }
}

static AxlkProc *
pcb_by_pid(AxlkPid pid)
{
    for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
        if (pcb_table[i].state != AXLK_PROC_FREE && pcb_table[i].pid == pid) {
            return &pcb_table[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Ready queue (FIFO)
// ---------------------------------------------------------------------------

static void
ready_push(AxlkProc *p)
{
    p->next_ready = NULL;
    if (ready_tail != NULL) {
        ready_tail->next_ready = p;
    } else {
        ready_head = p;
    }
    ready_tail = p;
}

static AxlkProc *
ready_pop(void)
{
    AxlkProc *p = ready_head;
    if (p == NULL) {
        return NULL;
    }
    ready_head = p->next_ready;
    if (ready_head == NULL) {
        ready_tail = NULL;
    }
    p->next_ready = NULL;
    return p;
}

// ---------------------------------------------------------------------------
// Sleep list (singly-linked, unordered — cheap for small N)
// ---------------------------------------------------------------------------

static void
sleep_list_add(AxlkProc *p)
{
    p->next_sleep = sleep_head;
    sleep_head = p;
}

/* sleep_list_remove not needed in POC — wake_sleepers unlinks inline. */

// ---------------------------------------------------------------------------
// Canary check — catches most stack-overflow bugs
// ---------------------------------------------------------------------------

static void
check_canary(AxlkProc *p)
{
    if (p->stack_base == NULL) {
        return;  /* pid 0 / scheduler has no pool stack */
    }
    if (*(volatile uint64_t *)p->stack_base != AXLK_STACK_CANARY) {
        axl_printf("kernel: stack overflow in pid %d (canary clobbered)\n",
                   (int)p->pid);
        /* Can't recover — overflowed proc's state is unknown. */
        axlk_exit(-1);
    }
}

// ---------------------------------------------------------------------------
// Trampoline — first code to run in a freshly-spawned process
// ---------------------------------------------------------------------------

static void
axlk_proc_trampoline(void)
{
    AxlkProc *self = current_proc;
    int rc = self->entry(self->argc, self->argv);
    axlk_exit(rc);
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlkPid
axlk_spawn(AxlkProcMain entry, int argc, char **argv, size_t stack_kib)
{
    if (entry == NULL) {
        return -1;
    }
    if (stack_kib != 0 && stack_kib != AXLK_POC_STACK_KIB) {
        axl_printf("kernel: non-default stack_kib=%zu rejected in POC\n",
                   stack_kib);
        return -1;
    }

    AxlkProc *p = pcb_alloc();
    if (p == NULL) {
        return -1;
    }

    int slot = stack_pool_alloc();
    if (slot < 0) {
        pcb_free(p);
        return -1;
    }

    p->stack_base = stack_pool[slot];
    p->stack_size = AXLK_POC_STACK_BYTES;
    p->stack_slot = slot;
    p->entry      = entry;
    p->argc       = argc;
    p->argv       = argv;

    /* Write the canary at the base (lowest address). */
    *(uint64_t *)p->stack_base = AXLK_STACK_CANARY;

    /* Set up the initial stack so ctx_switch's `ret` enters the
     * trampoline. Stack grows downward; top = base + size.
     *
     * The two ABIs differ on how `ret` works:
     *   x86-64: `ret` pops the return address off the stack. So we
     *     stash the trampoline address on the new stack and set
     *     ctx.rsp to point at it. Alignment: SysV requires
     *     (rsp + 8) % 16 == 0 at function entry — after `ret` pops
     *     8 bytes, entry rsp = ctx.rsp + 8, so ctx.rsp must be
     *     16-aligned. Missing this trips #GP on any movaps gcc
     *     emits for SSE-aligned locals.
     *   aarch64: `ret` branches to LR (x30). So we set ctx.x30 to
     *     the trampoline directly; no return-address-on-stack
     *     needed. AAPCS64 requires sp 16-aligned at function entry,
     *     which we just hand ctx.sp directly.
     */
    uintptr_t top = (uintptr_t)p->stack_base + p->stack_size;
    top &= ~((uintptr_t)15);       /* 16-align the top edge */

#if defined(__x86_64__)
    top -= 16;                      /* reserve one 16-aligned slot; ret reads lower 8 */
    *(uintptr_t *)top = (uintptr_t)axlk_proc_trampoline;
    p->ctx.rsp = top;               /* 16-aligned — ret will add 8 for entry rsp */
#elif defined(__aarch64__)
    p->ctx.sp  = top;                        /* 16-aligned */
    p->ctx.x30 = (uintptr_t)axlk_proc_trampoline; /* LR → ret branches here */
#endif
    /* Other callee-saved regs can stay zero — trampoline doesn't use them. */

    ready_push(p);
    return p->pid;
}

void
axlk_yield(void)
{
    if (current_proc == NULL) {
        return;
    }
    check_canary(current_proc);
    current_proc->state = AXLK_PROC_READY;
    ready_push(current_proc);

    AxlkProc *me = current_proc;
    current_proc = NULL;
    axlk_ctx_switch(&me->ctx, &sched_ctx);
    /* Resumed — scheduler has set current_proc back to me. */
}

void
axlk_sleep_ms(uint32_t ms)
{
    if (current_proc == NULL || ms == 0) {
        return;
    }
    check_canary(current_proc);

    AxlkProc *me = current_proc;
    me->state      = AXLK_PROC_WAITING;
    me->wake_at_ms = axl_time_get_ms() + ms;
    sleep_list_add(me);

    current_proc = NULL;
    axlk_ctx_switch(&me->ctx, &sched_ctx);
}

void
axlk_exit(int status)
{
    AxlkProc *self = current_proc;
    if (self == NULL) {
        /* Called from kernel / pre-run context — just bail. */
        axl_printf("kernel: axlk_exit(%d) from non-proc context\n", status);
        for (;;) { /* nothing sensible to do */ }
    }

    self->state       = AXLK_PROC_ZOMBIE;
    self->exit_status = status;

    /* Push onto the scheduler's zombie list. */
    self->next_ready = zombie_head;
    zombie_head = self;

    if (self->pid == 1) {
        pid1_exit_status = status;
    }

    /* Wake any parent that's axlk_wait()ing on us (or on AXLK_PID_ANY). */
    AxlkProc *parent = pcb_by_pid(self->ppid);
    if (parent != NULL && parent->state == AXLK_PROC_WAITING
        && (parent->wait_for_pid == AXLK_PID_ANY
            || parent->wait_for_pid == self->pid))
    {
        parent->state = AXLK_PROC_READY;
        parent->wait_for_pid = 0;
        ready_push(parent);
    }

    current_proc = NULL;
    axlk_ctx_switch(&self->ctx, &sched_ctx);
    /* Never reached — scheduler reaps us on its stack. */
    __builtin_unreachable();
}

AxlkPid
axlk_waitpid(AxlkPid pid, int *status, int flags)
{
    AxlkProc *self = current_proc;
    if (self == NULL) {
        return -1;
    }

    for (;;) {
        /* Look for an already-zombie child we can reap. */
        AxlkProc **cur = &zombie_head;
        while (*cur != NULL) {
            AxlkProc *z = *cur;
            if (z->ppid == self->pid
                && (pid == AXLK_PID_ANY || z->pid == pid))
            {
                AxlkPid zpid = z->pid;
                if (status != NULL) {
                    *status = z->exit_status;
                }
                *cur = z->next_ready;
                stack_pool_free(z->stack_slot);
                pcb_free(z);
                return zpid;
            }
            cur = &(*cur)->next_ready;
        }

        /* Any live children? If not, return -1. */
        bool have_child = false;
        for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
            AxlkProc *p = &pcb_table[i];
            if (p->state != AXLK_PROC_FREE && p->ppid == self->pid) {
                if (pid == AXLK_PID_ANY || p->pid == pid) {
                    have_child = true;
                    break;
                }
            }
        }
        if (!have_child) {
            return -1;
        }

        /* Matching child exists but isn't a zombie. POSIX WNOHANG
         * returns 0 here; blocking callers drop into the ctx switch. */
        if (flags & AXLK_WNOHANG) {
            return 0;
        }

        self->state        = AXLK_PROC_WAITING;
        self->wait_for_pid = pid;
        current_proc = NULL;
        axlk_ctx_switch(&self->ctx, &sched_ctx);
        /* Resumed — check the zombie list again. */
    }
}

AxlkPid
axlk_wait(AxlkPid pid, int *status)
{
    return axlk_waitpid(pid, status, 0);
}

AxlkPid
axlk_getpid(void)
{
    return current_proc != NULL ? current_proc->pid : 0;
}

AxlkPid
axlk_getppid(void)
{
    return current_proc != NULL ? current_proc->ppid : 0;
}

size_t
axlk_proc_count(void)
{
    size_t n = 0;
    for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
        AxlkProcState s = pcb_table[i].state;
        if (s != AXLK_PROC_FREE && s != AXLK_PROC_ZOMBIE) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// fd table helpers (K3)
// ---------------------------------------------------------------------------

static int
fd_alloc(AxlkFdKind kind, AxlTcp *tcp)
{
    /* index 0 reserved as invalid sentinel so returned fds are always positive */
    for (int i = 1; i < AXLK_MAX_FDS; i++) {
        if (fd_table[i].kind == AXLK_FD_FREE) {
            fd_table[i].kind = kind;
            fd_table[i].tcp  = tcp;
            return i;
        }
    }
    return -1;
}

static AxlkFdSlot *
fd_get(int fd, AxlkFdKind kind)
{
    if (fd < 1 || fd >= AXLK_MAX_FDS) {
        return NULL;
    }
    AxlkFdSlot *s = &fd_table[fd];
    if (s->kind != kind) {
        return NULL;
    }
    return s;
}

static void
fd_release(int fd)
{
    if (fd < 1 || fd >= AXLK_MAX_FDS) {
        return;
    }
    fd_table[fd].kind = AXLK_FD_FREE;
    fd_table[fd].tcp  = NULL;
}

// ---------------------------------------------------------------------------
// Syscall infrastructure (K3)
//
// Shape for any blocking syscall:
//   1. Mark current proc WAITING, stash anything the resume path needs.
//   2. Register an axl-sdk async op against sched_loop; pass `me` as data.
//   3. Context-switch to scheduler.
//   4. On resume (scheduler has set current_proc back to us), read
//      syscall_result and return it.
//
// The completion callback runs on the scheduler stack during
// axl_loop_dispatch_event. It writes syscall_result and marks the
// PCB ready. Never call axlk_* from inside a completion callback.
// ---------------------------------------------------------------------------

static void
syscall_suspend_and_switch(AxlkProc *me)
{
    me->state = AXLK_PROC_WAITING;
    current_proc = NULL;
    axlk_ctx_switch(&me->ctx, &sched_ctx);
    /* Resumed here. Scheduler restored current_proc = me. */
}

static void
syscall_wake(AxlkProc *p, intptr_t result)
{
    p->syscall_result = result;
    p->state          = AXLK_PROC_READY;
    ready_push(p);
}

// ---------------------------------------------------------------------------
// TCP syscalls (K3)
// ---------------------------------------------------------------------------

int
axlk_listen(uint16_t port)
{
    AxlTcp *listener = NULL;
    if (axl_tcp_listen(port, &listener) != 0 || listener == NULL) {
        return -1;
    }
    int fd = fd_alloc(AXLK_FD_TCP_LISTENER, listener);
    if (fd < 0) {
        axl_tcp_close(listener);
        return -1;
    }
    return fd;
}

static bool
on_accept_complete(AxlTcp *client, int status, void *data)
{
    AxlkProc *p = (AxlkProc *)data;
    intptr_t result = -1;
    if (status == 0 && client != NULL) {
        int fd = fd_alloc(AXLK_FD_TCP_CONN, client);
        if (fd < 0) {
            axl_tcp_close(client);
        } else {
            result = fd;
        }
    }
    syscall_wake(p, result);
    return false;   /* one-shot per axlk_accept call */
}

int
axlk_accept(int listen_fd)
{
    AxlkFdSlot *slot = fd_get(listen_fd, AXLK_FD_TCP_LISTENER);
    if (slot == NULL) {
        return -1;
    }
    AxlkProc *me = current_proc;
    if (me == NULL) {
        return -1;
    }

    if (axl_tcp_accept_async(slot->tcp, sched_loop, NULL,
                             on_accept_complete, me) != 0) {
        return -1;
    }
    syscall_suspend_and_switch(me);
    return (int)me->syscall_result;
}

static bool
on_recv_complete(AxlTcp *sock, int status, void *data)
{
    AxlkProc *p = (AxlkProc *)data;
    intptr_t  result;
    if (status == 0) {
        result = (intptr_t)axl_tcp_recv_get_size(sock);
    } else if (status == AXL_CANCELLED) {
        result = -1;
    } else {
        /* Peer closed or error — return 0 bytes as EOF. */
        result = 0;
    }
    syscall_wake(p, result);
    return false;
}

int
axlk_read(int fd, void *buf, size_t n)
{
    AxlkFdSlot *slot = fd_get(fd, AXLK_FD_TCP_CONN);
    if (slot == NULL || buf == NULL || n == 0) {
        return -1;
    }
    AxlkProc *me = current_proc;
    if (me == NULL) {
        return -1;
    }

    if (axl_tcp_recv_async(slot->tcp, buf, n, sched_loop, NULL,
                           on_recv_complete, me) != 0) {
        return -1;
    }
    syscall_suspend_and_switch(me);
    return (int)me->syscall_result;
}

static bool
on_send_complete(AxlTcp *sock, int status, void *data)
{
    (void)sock;
    AxlkProc *p = (AxlkProc *)data;
    syscall_wake(p, (status == 0) ? 0 : -1);
    return false;
}

int
axlk_write(int fd, const void *buf, size_t n)
{
    AxlkFdSlot *slot = fd_get(fd, AXLK_FD_TCP_CONN);
    if (slot == NULL || buf == NULL || n == 0) {
        return -1;
    }
    AxlkProc *me = current_proc;
    if (me == NULL) {
        return -1;
    }

    if (axl_tcp_send_async(slot->tcp, buf, n, sched_loop, NULL,
                           on_send_complete, me) != 0) {
        return -1;
    }
    syscall_suspend_and_switch(me);
    return (int)me->syscall_result;
}

void
axlk_close(int fd)
{
    if (fd < 1 || fd >= AXLK_MAX_FDS) {
        return;
    }
    AxlkFdSlot *s = &fd_table[fd];
    if (s->kind == AXLK_FD_FREE) {
        return;
    }
    if (s->tcp != NULL) {
        axl_tcp_close(s->tcp);
    }
    fd_release(fd);
}

// ---------------------------------------------------------------------------
// K3.1 — HTTP convenience: read + parse request line
// ---------------------------------------------------------------------------

int
axlk_http_read_request_line(
    int     fd,
    char   *scratch,
    size_t  scratch_cap,
    char   *method_out,
    size_t  method_cap,
    char   *path_out,
    size_t  path_cap)
{
    if (scratch == NULL || scratch_cap < 4 ||
        method_out == NULL || method_cap == 0 ||
        path_out == NULL || path_cap == 0) {
        return -1;
    }

    /* Read until we see the header terminator or the buffer fills. */
    size_t total = 0;
    while (total < scratch_cap - 1) {
        int n = axlk_read(fd, scratch + total, scratch_cap - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        scratch[total] = '\0';
        if (axl_http_find_header_end(scratch, total) > 0) {
            break;
        }
    }
    if (axl_http_find_header_end(scratch, total) == 0) {
        return -1;
    }

    /* Locate the end of the request line (first \r\n in the buffer). */
    size_t line_len = 0;
    while (line_len + 1 < total &&
           !(scratch[line_len] == '\r' && scratch[line_len + 1] == '\n')) {
        line_len++;
    }
    if (line_len + 1 >= total) {
        return -1;
    }

    /* Delegate parsing to the axl-sdk public helper. */
    AXL_AUTO_FREE char *method = NULL;
    AXL_AUTO_FREE char *path   = NULL;
    AXL_AUTO_FREE char *query  = NULL;
    if (axl_http_parse_request_line(scratch, line_len,
                                    &method, &path, &query) != 0) {
        return -1;
    }

    axl_strlcpy(method_out, method != NULL ? method : "", method_cap);
    axl_strlcpy(path_out,   path   != NULL ? path   : "", path_cap);
    return 0;
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

static void
reap_zombies(void)
{
    /* Only unclaimed zombies end up here — axlk_wait pulls reaped ones
     * off the list already. Unclaimed zombies (no parent waiting, or
     * orphans) get collected here to release their slots. */
    AxlkProc **cur = &zombie_head;
    while (*cur != NULL) {
        AxlkProc *z = *cur;
        AxlkProc *parent = pcb_by_pid(z->ppid);
        if (parent == NULL || parent->state == AXLK_PROC_FREE) {
            /* Parent already gone — reap now. */
            *cur = z->next_ready;
            stack_pool_free(z->stack_slot);
            pcb_free(z);
        } else {
            cur = &(*cur)->next_ready;
        }
    }
}

static void
wake_sleepers(void)
{
    if (sleep_head == NULL) {
        return;    /* avoid the UEFI GetTime call in the hot path */
    }
    uint64_t now = axl_time_get_ms();
    AxlkProc **cur = &sleep_head;
    while (*cur != NULL) {
        AxlkProc *p = *cur;
        if (p->state == AXLK_PROC_WAITING
            && p->wake_at_ms != 0
            && now >= p->wake_at_ms)
        {
            p->wake_at_ms = 0;
            p->state      = AXLK_PROC_READY;
            *cur = p->next_sleep;
            p->next_sleep = NULL;
            ready_push(p);
        } else {
            cur = &(*cur)->next_sleep;
        }
    }
}

static void
scheduler_main(void)
{
    for (;;) {
        reap_zombies();
        wake_sleepers();

        AxlkProc *next = ready_pop();
        if (next == NULL) {
            /* Nothing ready. If pid 1 is gone and no one else is
             * sleeping/waiting, we're done. */
            bool any_alive = false;
            for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
                AxlkProcState s = pcb_table[i].state;
                if (s == AXLK_PROC_READY || s == AXLK_PROC_RUNNING
                    || s == AXLK_PROC_WAITING) {
                    any_alive = true;
                    break;
                }
            }
            if (!any_alive) {
                /* Reap any leftover orphan zombies before returning. */
                while (zombie_head != NULL) {
                    AxlkProc *z = zombie_head;
                    zombie_head = z->next_ready;
                    stack_pool_free(z->stack_slot);
                    pcb_free(z);
                }
                return;
            }
            /* Idle: drive sched_loop for one event. This is the real
             * host-friendly wait — gBS->WaitForEvent blocks until a
             * registered source fires (TCP completion, timer, shell
             * break). No busy-spin. If no sources are registered we
             * fall back to a short sleep to avoid a tight empty loop;
             * this only happens if all live procs are in axlk_wait
             * (which doesn't register a loop source). */
            int lr = axl_loop_next_event(sched_loop, true);
            if (lr == 0) {
                axl_loop_dispatch_event(sched_loop);
            } else if (lr == -1) {
                /* Ctrl-C observed by the loop — abort. */
                axl_printf("kernel: shell break observed, tearing down\n");
                return;
            } else {
                /* No sources + blocking=true shouldn't normally hit,
                 * but if it does we want a host-friendly sleep. */
                axl_usleep(1000);
            }
            continue;
        }

        check_canary(next);
        current_proc = next;
        next->state  = AXLK_PROC_RUNNING;
        axlk_ctx_switch(&sched_ctx, &next->ctx);
        /* Back on scheduler stack. `next` is now READY / WAITING /
         * ZOMBIE depending on why it gave up the CPU. */
    }
}

int
axlk_init(void)
{
    if (kernel_initialized) {
        return 0;
    }
    for (int i = 0; i < AXLK_POC_MAX_PROCS; i++) {
        pcb_table[i].state = AXLK_PROC_FREE;
        stack_used[i] = false;
    }
    for (int i = 0; i < AXLK_MAX_FDS; i++) {
        fd_table[i].kind = AXLK_FD_FREE;
        fd_table[i].tcp  = NULL;
    }
    ready_head = ready_tail = NULL;
    zombie_head = NULL;
    sleep_head  = NULL;
    current_proc = NULL;
    next_pid_val = 1;
    pid1_exit_status = 0;

    sched_loop = axl_loop_new();
    if (sched_loop == NULL) {
        return -1;
    }

    kernel_initialized = true;
    return 0;
}

int
axlk_run(AxlkProcMain entry, int argc, char **argv)
{
    if (!kernel_initialized || entry == NULL) {
        return -1;
    }

    AxlkPid pid = axlk_spawn(entry, argc, argv, 0);
    if (pid < 0) {
        return -1;
    }
    /* pid 1 convention: first spawn gets pid 1. (next_pid_val starts at 1.) */

    scheduler_main();

    /* Kernel-wide teardown: close any fds still open, then free loop. */
    for (int i = 1; i < AXLK_MAX_FDS; i++) {
        if (fd_table[i].kind != AXLK_FD_FREE && fd_table[i].tcp != NULL) {
            axl_tcp_close(fd_table[i].tcp);
            fd_table[i].kind = AXLK_FD_FREE;
            fd_table[i].tcp  = NULL;
        }
    }
    if (sched_loop != NULL) {
        axl_loop_free(sched_loop);
        sched_loop = NULL;
    }
    kernel_initialized = false;

    return pid1_exit_status;
}
