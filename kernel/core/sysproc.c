/**
 * @file sysproc.c
 * @brief System call handlers implementation.
 */

#include "core/types.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/vm.h"

/**
 * @brief Exit system call handler.
 *
 * @return Does not return.
 */
uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

/**
 * @brief Getpid system call handler.
 *
 * @return Current process PID.
 */
uint64
sys_getpid(void)
{
  return myproc()->pid;
}

/**
 * @brief Fork system call handler.
 *
 * @return PID of child process to parent, 0 to child.
 */
uint64
sys_fork(void)
{
  return kfork();
}

/**
 * @brief Wait system call handler.
 *
 * @return PID of exited child, or -1 if no children.
 */
uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

/**
 * @brief Sbrk system call handler.
 *
 * @return Old process size on success, -1 on failure.
 */
uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz = addr + n;
  }

  return addr;
}

/**
 * @brief Pause system call handler (sleep).
 *
 * @return Does not return until awakened.
 */
uint64
sys_pause(void)
{
  sleep(0, 0);
  return 0;
}

/**
 * @brief Gettimeofday/uptime system call handler.
 *
 * @return Number of clock ticks since boot.
 */
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

/**
 * @brief Kill system call handler.
 *
 * @return 0 on success, -1 if process not found.
 */
uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}
