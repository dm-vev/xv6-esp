/**
 * @file sleeplock.h
 * @brief Long-term locks (sleep locks) interface.
 */
#ifndef SLEEPLOCK_H
#define SLEEPLOCK_H

/**
 * @brief Long-term lock for processes.
 *
 * Unlike spinlocks, sleep locks allow the CPU to be released while waiting.
 * Used for resources that may be held for extended periods.
 */
struct sleeplock {
  uint locked;       // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock
  
  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

#endif // SLEEPLOCK_H
