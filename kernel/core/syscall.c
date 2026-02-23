/**
 * @file syscall.c
 * @brief System call handling implementation.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/syscall.h"
#include "core/defs.h"

/**
 * @brief Fetches a uint64 value from user memory.
 *
 * @param addr User virtual address to read from.
 * @param ip   Pointer to store the fetched value.
 *
 * @pre addr must be within process memory bounds.
 * @pre addr+sizeof(uint64) must not overflow.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if address is out of bounds.
 * @error Returns -1 if copyin fails.
 */
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz) // both tests needed, in case of overflow
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

/**
 * @brief Fetches a null-terminated string from user memory.
 *
 * @param addr User virtual address of string.
 * @param buf  Buffer to store the string.
 * @param max  Maximum bytes to copy (including null terminator).
 *
 * @return Length of string (not including null) on success.
 * @return -1 on error.
 *
 * @error Returns -1 if copyinstr fails.
 */
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  return strlen(buf);
}

/**
 * @brief Fetches raw argument value from trapframe.
 *
 * @param n Argument index (0-5).
 *
 * @pre n must be between 0 and 5 inclusive.
 *
 * @return Value of nth argument register.
 *
 * @error Panics if n is out of range.
 */
static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

/**
 * @brief Fetches the nth system call argument as an integer.
 *
 * @param n  Argument index (0-5).
 * @param ip Pointer to store the integer value.
 *
 * @post *ip contains the integer value of argument n.
 *
 * @return None.
 */
void
argint(int n, int *ip)
{
  *ip = argraw(n);
}

/**
 * @brief Fetches the nth system call argument as an address.
 *
 * @param n  Argument index (0-5).
 * @param ip Pointer to store the address value.
 *
 * @post *ip contains the address value of argument n.
 *
 * @note Does not check validity - copyin/copyout handle that.
 *
 * @return None.
 */
void
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
}

/**
 * @brief Fetches the nth word-sized system call argument as a null-terminated string.
 * Copies into buf, at most max.
 * Returns string length if OK (including nul), -1 if error.
 */
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);

/**
 * @brief System call dispatch table.
 *
 * Maps syscall numbers from syscall.h to handler functions.
 */
static uint64 (*syscalls[])(void) = {
  0,
  sys_fork,
  sys_exit,
  sys_wait,
  sys_pipe,
  sys_read,
  sys_kill,
  sys_exec,
  sys_fstat,
  sys_chdir,
  sys_dup,
  sys_getpid,
  sys_sbrk,
  sys_pause,
  sys_uptime,
  sys_open,
  sys_write,
  sys_mknod,
  sys_unlink,
  sys_link,
  sys_mkdir,
  sys_close,
};

/**
 * @brief System call handler dispatch function.
 *
 * Called from trap.c when a system call trap occurs.
 * Looks up the system call function by number and executes it.
 *
 * @post System call function result stored in a0 register.
 *
 * @note Prints error message for unknown system calls.
 *
 * @return None.
 */
void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // Use num to lookup the system call function for num, call it,
    // and store its return value in p->trapframe->a0
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
