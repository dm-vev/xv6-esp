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
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
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
  push_off(); // disable interrupts to avoid deadlock.
  if(holding(lk))
    panic("acquire");

  // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // Tell the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Record info about lock acquisition for holding() and debugging.
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
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
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
  int old = intr_get();

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();

  if(mycpu()->noff == 0)
    mycpu()->intena = old;
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
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
