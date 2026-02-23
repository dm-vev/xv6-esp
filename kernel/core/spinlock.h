/**
 * @file spinlock.h
 * @brief Mutual exclusion lock interface.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

/**
 * @brief Mutual exclusion lock.
 */
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
};

#endif // SPINLOCK_H
