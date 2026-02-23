/**
 * @file sleeplock.c
 * @brief Sleeping locks implementation.
 */

#include "core/types.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/sleeplock.h"

/**
 * @brief Initializes a sleep lock.
 *
 * @param lk     Pointer to the sleep lock to initialize.
 * @param name   Name string for debugging purposes.
 *
 * @note The lock is initially in the unlocked state.
 * @note The underlying spinlock is also initialized.
 *
 * @return None.
 */
void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

/**
 * @brief Acquires a sleep lock.
 *
 * @param lk Pointer to the sleep lock to acquire.
 *
 * @pre Interrupts must be enabled.
 *
 * @post The lock is held by the calling process.
 * @post The PID of the holding process is recorded.
 *
 * @note Unlike spinlocks, this releases the CPU while waiting.
 * @note Uses sleep() to yield the CPU until lock is available.
 *
 * @return None.
 */
void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

/**
 * @brief Releases a sleep lock.
 *
 * @param lk Pointer to the sleep lock to release.
 *
 * @pre The lock must be held by some process.
 *
 * @post The lock is released (locked = 0).
 * @post All waiters are woken up via wakeup().
 *
 * @note Wakes up all processes waiting on this lock.
 *
 * @return None.
 */
void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

/**
 * @brief Checks whether the current process holds the sleep lock.
 *
 * @param lk Pointer to the sleep lock to check.
 *
 * @return 1 if the lock is held by the current process, 0 otherwise.
 *
 * @note Acquires and releases the underlying spinlock internally.
 */
int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}
