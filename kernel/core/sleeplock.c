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
  // Initialize the underlying spinlock that protects sleep lock state
  initlock(&lk->lk, "sleep lock");
  
  // Set the debugging name for this lock
  lk->name = name;
  
  // Initialize as unlocked (0 = free)
  lk->locked = 0;
  
  // No process holds the lock initially
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
  // Acquire the underlying spinlock to modify sleep lock state
  acquire(&lk->lk);
  
  // While lock is held by another process, sleep and release spinlock
  // This allows other processes to run while waiting
  while (lk->locked) {
    sleep(lk, &lk->lk);  // Sleep on this lock, release spinlock
  }
  
  // Lock is now free - mark as held
  lk->locked = 1;
  
  // Record our PID so we can check ownership later
  lk->pid = myproc()->pid;
  
  // Release the underlying spinlock - we now own the sleep lock
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
  // Acquire underlying spinlock to modify state
  acquire(&lk->lk);
  
  // Mark lock as free
  lk->locked = 0;
  
  // Clear the holder PID
  lk->pid = 0;
  
  // Wake up all processes waiting on this lock
  wakeup(lk);
  
  // Release the underlying spinlock
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
  
  // Must acquire spinlock to safely read sleep lock state
  acquire(&lk->lk);
  
  // Check if lock is held AND held by current process
  r = lk->locked && (lk->pid == myproc()->pid);
  
  // Release spinlock after reading state
  release(&lk->lk);
  
  return r;
}
