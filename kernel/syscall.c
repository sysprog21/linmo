#include <hal.h>
#include <sys/errno.h>
#include <sys/syscall.h>
#include <sys/task.h>

#include "private/stdio.h"
#include "private/task.h"
#include "private/utils.h"

/* syscall wrappers - forward declarations */
#define _(name, num, rettype, arglist) static rettype _##name arglist;
SYSCALL_TABLE
#undef _

/* syscall table */
#define _(name, num, ret, args) [num] = (void *) _##name,
static const void *syscall_table[SYS_COUNT] = {SYSCALL_TABLE};
#undef _

/* Core syscall execution via direct table lookup.
 * Called by trap handlers to invoke syscall implementations without
 * triggering privilege transitions. User space must not call this directly.
 *
 * Per-task syscall context tracking:
 * - Sets tcb_t.in_syscall flag for current task (survives preemption)
 * - Enables runtime privilege checks in kernel APIs (defense-in-depth)
 */
int do_syscall(int num, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    if (unlikely(num <= 0 || num >= SYS_COUNT))
        return -ENOSYS;

    if (unlikely(!syscall_table[num]))
        return -ENOSYS;

    /* Per-task syscall context tracking (survives preemption) */
    tcb_t *self = (kcb && kcb->task_current) ? kcb->task_current->data : NULL;

    if (self)
        self->in_syscall = true;

    int result =
        ((int (*)(uintptr_t, uintptr_t, uintptr_t)) syscall_table[num])(a1, a2,
                                                                        a3);

    if (self)
        self->in_syscall = false;
    return result;
}

/* Generic user-space syscall interface.
 * This weak symbol allows architecture-specific implementations to override
 * with trap-based entry mechanisms.
 */
__attribute__((weak)) int syscall(int num,
                                  uintptr_t a1,
                                  uintptr_t a2,
                                  uintptr_t a3)
{
    return do_syscall(num, a1, a2, a3);
}

static char *_env[1] = {0};
char **environ = _env;
int errno = 0;

/* UNIX syscalls (dummy implementation) */

static int _fork(void)
{
    errno = EAGAIN;
    return -1;
}

static int _exit(int status)
{
    _kill(status, -1);
    while (1)
        ;

    return -1;
}

static int _wait(int *status)
{
    errno = ECHILD;
    return -1;
}

static int _pipe(int fildes[2])
{
    errno = EFAULT;
    return -1;
}

static int _kill(int pid, int sig)
{
    errno = EINVAL;
    return -1;
}

static int _execve(char *name, char **argv, char **env)
{
    errno = ENOMEM;
    return -1;
}

static int _dup(int oldfd)
{
    errno = EBADF;
    return -1;
}

static int _getpid(void)
{
    return 1;
}

/* linmo uses fixed heap via mo_heap_init, sbrk is not applicable */
static int _sbrk(int incr)
{
    (void) incr;
    errno = ENOMEM;
    return -1;
}

static int _usleep(int usec)
{
    if (unlikely(usec < 0)) {
        errno = EINVAL;
        return -1;
    }

    errno = EINTR;
    return 0;
}

static int _stat(char *file, struct stat *st)
{
    if (unlikely(!st)) {
        errno = EFAULT;
        return -1;
    }

    st->st_mode = S_IFCHR;
    return 0;
}

static int _open(char *path, int flags)
{
    errno = ENOENT;
    return -1;
}

static int _close(int file)
{
    if (unlikely(file < 0)) {
        errno = EBADF;
        return -1;
    }
    return -1;
}

static int _read(int file, char *ptr, int len)
{
    if (unlikely(!ptr || len < 0)) {
        errno = EFAULT;
        return -1;
    }

    if (unlikely(file < 0 || file > 2)) {
        errno = EBADF;
        return -1;
    }

    int idx;
    for (idx = 0; idx < len; idx++)
        *ptr++ = _getchar();

    return len;
}

static int _write(int file, char *ptr, int len)
{
    if (unlikely(!ptr || len < 0)) {
        errno = EFAULT;
        return -1;
    }

    if (unlikely(file < 0 || file > 2)) {
        errno = EBADF;
        return -1;
    }

    int idx;
    for (idx = 0; idx < len; idx++)
        _putchar(*ptr++);

    return len;
}

static int _lseek(int file, int ptr, int dir)
{
    if (unlikely(file < 0)) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

static int _chdir(const char *path)
{
    if (unlikely(!path)) {
        errno = EFAULT;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

static int _mknod(const char *path, int mode, int dev)
{
    if (unlikely(!path)) {
        errno = EFAULT;
        return -1;
    }
    errno = EPERM;
    return -1;
}

static int _unlink(char *name)
{
    if (unlikely(!name)) {
        errno = EFAULT;
        return -1;
    }
    errno = ENOENT;
    return -1;
}

static int _link(char *old, char *new)
{
    if (unlikely(!old || !new)) {
        errno = EFAULT;
        return -1;
    }
    errno = EMLINK;
    return -1;
}

/* Linmo syscalls (wrapper implementation) */

static int _task_spawn(void *task, int stack_size)
{
    if (unlikely(!task || stack_size <= 0))
        return -EINVAL;

    /* Syscall always creates U-mode tasks for security */
    return mo_task_spawn_user(task, stack_size);
}

int sys_task_spawn(void *task, int stack_size)
{
    return syscall(SYS_task_spawn, (uintptr_t) task, (uintptr_t) stack_size, 0);
}

static int _tcancel(int id)
{
    if (unlikely(id <= 0 || id > (int) UINT16_MAX))
        return -EINVAL;
    if ((uint16_t) id != mo_task_id())
        return -EPERM;

    return mo_task_cancel(id);
}

int sys_tcancel(int id)
{
    return syscall(SYS_tcancel, (uintptr_t) id, 0, 0);
}

static int _tyield(void)
{
    mo_task_yield();
    return 0;
}

int sys_tyield(void)
{
    return syscall(SYS_tyield, 0, 0, 0);
}

static int _tdelay(int ticks)
{
    if (unlikely(ticks < 0))
        return -EINVAL;

    mo_task_delay(ticks);
    return 0;
}

int sys_tdelay(int ticks)
{
    return syscall(SYS_tdelay, (uintptr_t) ticks, 0, 0);
}

static int _tsuspend(int id)
{
    if (unlikely(id <= 0 || id > (int) UINT16_MAX))
        return -EINVAL;
    if ((uint16_t) id != mo_task_id())
        return -EPERM;

    return mo_task_suspend(id);
}

int sys_tsuspend(int id)
{
    return syscall(SYS_tsuspend, (uintptr_t) id, 0, 0);
}

static int _tresume(int id)
{
    (void) id;
    /* U-mode cannot resume any task; suspended task cannot call syscall */
    return -EPERM;
}

int sys_tresume(int id)
{
    return syscall(SYS_tresume, (uintptr_t) id, 0, 0);
}

static int _tpriority(int id, int priority)
{
    if (unlikely(id <= 0 || id > (int) UINT16_MAX))
        return -EINVAL;
    if ((uint16_t) id != mo_task_id())
        return -EPERM;

    return mo_task_priority(id, priority);
}

int sys_tpriority(int id, int priority)
{
    return syscall(SYS_tpriority, (uintptr_t) id, (uintptr_t) priority, 0);
}

static int _tid(void)
{
    return mo_task_id();
}

int sys_tid(void)
{
    return syscall(SYS_tid, 0, 0, 0);
}

static int _twfi(void)
{
    mo_task_wfi();
    return 0;
}

int sys_twfi(void)
{
    return syscall(SYS_twfi, 0, 0, 0);
}

static int _tcount(void)
{
    return mo_task_count();
}

int sys_tcount(void)
{
    return syscall(SYS_tcount, 0, 0, 0);
}

static int _ticks(void)
{
    return mo_ticks();
}

int sys_ticks(void)
{
    return syscall(SYS_ticks, 0, 0, 0);
}

static int _uptime(void)
{
    return mo_uptime();
}

int sys_uptime(void)
{
    return syscall(SYS_uptime, 0, 0, 0);
}

/* User mode safe output syscall.
 * Outputs a string from user mode directly via UART, bypassing the logger
 * queue. Direct output ensures strict ordering for U-mode tasks and avoids race
 * conditions with the async logger task.
 */
static int _tputs(const char *str)
{
    if (unlikely(!str))
        return -EINVAL;

    /* Prevent task switching during output to avoid character interleaving.
     * Ensures the entire string is output atomically with respect to other
     * tasks. Limit output length to prevent unbounded blocking.
     */
    NOSCHED_ENTER();
    const char *p;
    for (p = str; *p && (p - str) < 253; p++)
        _putchar(*p);
    if (*p) /* Truncated */
        _putchar('.'), _putchar('.'), _putchar('.');
    NOSCHED_LEAVE();

    return 0;
}

int sys_tputs(const char *str)
{
    return syscall(SYS_tputs, (uintptr_t) str, 0, 0);
}
