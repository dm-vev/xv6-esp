/**
 * @file spinlock.c
 * @brief Mutual exclusion spin locks implementation.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "core/spinlock.h"
#include "arch/riscv.h"
#include "core/proc.h"
#include "core/defs.h"

/**
 * @brief Initializes a spinlock.
 *
 * @param lk     Pointer to the spinlock to initialize.
 * @param name   Name string for debugging purposes.
 *
 * @note The lock is initially in the unlocked state (locked = 0).
 * @note Must be called before acquiring the lock.
 *
 * @return None.
 */
void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;      // Set the name for debugging
  lk->locked = 0;       // Initialize as unlocked (0 = free, 1 = held)
  lk->cpu = 0;          // No CPU holds the lock initially
}

/**
 * @brief Acquires the spinlock, spinning until successful.
 *
 * @param lk Pointer to the spinlock to acquire.
 *
 * @pre Interrupts must be enabled on entry.
 * @pre The calling thread must not already hold this lock.
 *
 * @post The lock is held by the calling CPU.
 * @post Interrupts are disabled while the lock is held.
 *
 * @note Uses atomic swap instruction (amoswap) on RISC-V.
 * @note Disables interrupts to prevent deadlock.
 *
 * @return None.
 *
 * @error Panics if the lock is already held by the current CPU.
 */
void
acquire(struct spinlock *lk)
{
  push_off(); // Disable interrupts to avoid deadlock - prevents context switch while holding lock
  if(holding(lk))  // Check if we already hold this lock - would cause deadlock
    panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  // Atomically: swap 1 into locked, return old value
  // Spin until old value was 0 (lock was free)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;  // Spin - lock is held by another CPU

  // Memory barrier - prevents reordering of memory operations
  // Ensures all memory reads/writes happen after lock acquisition
  // On RISC-V: generates fence instruction
  __sync_synchronize();

  // Record which CPU holds this lock - needed for debugging and holding() check
  lk->cpu = mycpu();
}

/**
 * @brief Releases the spinlock.
 *
 * @param lk Pointer to the spinlock to release.
 *
 * @pre The lock must be held by the calling CPU.
 *
 * @post The lock is released (locked = 0).
 * @post Interrupts are restored to their previous state.
 *
 * @note Uses atomic swap instruction (amoswap.w) on RISC-V.
 * @note Emits a memory fence to ensure all stores in critical section
 *       are visible before the lock is released.
 *
 * @return None.
 *
 * @error Panics if the lock is not held by the current CPU.
 */
void
release(struct spinlock *lk)
{
  if(!holding(lk))  // Verify we actually hold the lock - prevents accidental release
    panic("release");

  // Clear CPU holder - lock is no longer held
  lk->cpu = 0;

  // Memory barrier - ensures all our writes are visible before releasing lock
  // Prevents reordering of writes past the lock release
  __sync_synchronize();

  // Release the lock atomically
  // Using atomic release instead of simple assignment ensures
  // the release is visible to other CPUs immediately
  // On RISC-V: amoswap.w zero, zero, (s1) - atomic write of 0
  __sync_lock_release(&lk->locked);

  pop_off();  // Restore interrupt state to what it was before acquire()
}

/**
 * @brief Checks whether the current CPU holds the spinlock.
 *
 * @param lk Pointer to the spinlock to check.
 *
 * @pre Interrupts must be off when calling this function.
 *
 * @return 1 if the lock is held by the current CPU, 0 otherwise.
 *
 * @note Used for debugging and assertion checking.
 */
int
holding(struct spinlock *lk)
{
  int r;
  // Check both: lock is marked as held AND held by current CPU
  // Both conditions must be true for valid lock ownership
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

/**
 * @brief Decrements the interrupt disable nesting count.
 *
 * @pre Must be called after push_off().
 *
 * @post Interrupts are re-enabled if this is the final pop_off()
 *       and they were originally enabled.
 *
 * @note This is like intr_off()/intr_on() except matched in pairs:
 *       two push_off()s require two pop_off()s to undo.
 * @note If interrupts were initially off, push_off/pop_off leaves them off.
 *
 * @return None.
 *
 * @error Panics if called with interrupts already enabled.
 * @error Panics if the nesting count goes negative.
 */
void
push_off(void)
{
  int old = intr_get();  // Save current interrupt state

  // Disable interrupts - prevents involuntary context switch
  // while using mycpu() to get current CPU structure
  intr_off();

  // If this is first push_off() call, remember original interrupt state
  if(mycpu()->noff == 0)
    mycpu()->intena = old;  // Save whether interrupts were enabled
  
  // Increment nesting count - tracks how many times we've disabled interrupts
  mycpu()->noff += 1;
}

/**
 * @brief Increments the interrupt disable nesting count.
 *
 * @pre Must be called after push_off().
 *
 * @post Interrupts are re-enabled if this is the final pop_off()
 *       and they were originally enabled.
 *
 * @note This is like intr_off()/intr_on() except matched in pairs:
 *       two push_off()s require two pop_off()s to undo.
 *
 * @return None.
 *
 * @error Panics if called with interrupts already enabled.
 * @error Panics if the nesting count goes negative.
 */
void
pop_off(void)
{
  struct cpu *c = mycpu();  // Get current CPU structure
  
  // Panic if interrupts are already enabled - would indicate imbalance
  if(intr_get())
    panic("pop_off - interruptible");
  
  // Panic if noff is 0 or negative - would indicate imbalance
  if(c->noff < 1)
    panic("pop_off");
  
  // Decrement nesting count
  c->noff -= 1;
  
  // Only re-enable interrupts if:
  // 1. This was the final pop_off() (noff reached 0)
  // 2. Interrupts were originally enabled (intena was true)
  if(c->noff == 0 && c->intena)
    intr_on();
}
