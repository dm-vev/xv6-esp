/**
 * @file proc.c
 * @brief Process management implementation.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/memlayout.h"
#include "arch/riscv.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "core/defs.h"

/**
 * @brief Array of CPU structures, one per CPU.
 */
struct cpu cpus[NCPU];

/**
 * @brief Process table containing all processes.
 */
struct proc proc[NPROC];

/**
 * @brief Pointer to the init process (first user process).
 */
struct proc *initproc;

/**
 * @brief Next available PID.
 */
int nextpid = 1;

/**
 * @brief Spinlock protecting PID allocation.
 */
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

/**
 * @brief Spinlock protecting parent-child relationship during wait().
 *
 * Helps ensure that wakeups of wait()ing parents are not lost.
 * Must be acquired before any p->lock.
 */
struct spinlock wait_lock;

/**
 * @brief Allocates kernel stacks for all processes.
 *
 * Maps each process's kernel stack at a high virtual address,
 * followed by an invalid guard page.
 *
 * @param kpgtbl Kernel page table to use for mapping.
 *
 * @pre kpgtbl must be a valid kernel page table.
 * @post Each process has a kernel stack allocated and mapped.
 *
 * @note Uses kalloc() to get physical memory for each stack.
 * @note Stacks are mapped with read and write permissions.
 *
 * @return None.
 *
 * @error Panics if kalloc fails.
 */
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

/**
 * @brief Initializes the process table.
 *
 * Sets up locks for process management and initializes
 * each process slot to the UNUSED state.
 *
 * @note Initializes pid_lock and wait_lock spinlocks.
 * @note Sets initial kernel stack address for each process.
 *
 * @return None.
 */
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

/**
 * @brief Returns the ID of the current CPU.
 *
 * @pre Must be called with interrupts disabled to prevent
 *      race with process being moved to a different CPU.
 *
 * @return CPU ID (hartid from RISC-V tp register).
 */
int
cpuid()
{
  int id = r_tp();
  return id;
}

/**
 * @brief Returns a pointer to the current CPU structure.
 *
 * @pre Interrupts must be disabled.
 *
 * @return Pointer to the current cpu structure.
 */
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

/**
 * @brief Returns a pointer to the current process, or zero if none.
 *
 * @post Properly saves and restores interrupt state.
 *
 * @return Pointer to the current proc structure, or 0 if no process
 *         is running on this CPU.
 */
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

/**
 * @brief Atomically allocates and returns a new PID.
 *
 * @post PID is allocated atomically using pid_lock.
 *
 * @return The newly allocated PID.
 */
int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

/**
 * @brief Allocates a free process structure from the process table.
 *
 * Searches for an UNUSED process slot and initializes it
 * with a new PID, trapframe, and page table.
 *
 * @pre No locks held.
 * @post On success, returns with p->lock held.
 *
 * @return Pointer to allocated process on success, NULL on failure.
 *
 * @error Returns NULL if no free process slots available.
 * @error Returns NULL if trapframe allocation fails.
 * @error Returns NULL if page table allocation fails.
 */
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

/**
 * @brief Frees a process structure and all associated resources.
 *
 * @param p Pointer to the process to free.
 *
 * @pre p->lock must be held.
 *
 * @post Trapframe and page table are freed.
 * @post Process state set to UNUSED.
 *
 * @return None.
 */
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

/**
 * @brief Creates a user page table for a new process.
 *
 * Sets up an empty page table with trampoline code and
 * trapframe mappings.
 *
 * @param p Pointer to the process being created.
 *
 * @post Page table is allocated and mapped with trampoline and trapframe.
 *
 * @return New page table on success, NULL on failure.
 */
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

/**
 * @brief Frees a process's page table and associated physical memory.
 *
 * @param pagetable Page table to free.
 * @param sz Size of user memory in bytes.
 *
 * @post Trampoline and trapframe pages are unmapped.
 * @post User memory is freed.
 *
 * @return None.
 */
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

/**
 * @brief Creates and initializes the first user process.
 *
 * @post initproc points to the new process.
 * @post Process is in RUNNABLE state with root filesystem as cwd.
 *
 * @return None.
 */
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

/**
 * @brief Grows or shrinks user memory by n bytes.
 *
 * @param n Number of bytes to add (positive) or remove (negative).
 *
 * @pre Must be called from a valid process context.
 *
 * @post Process memory size is updated.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if new size would exceed TRAPFRAME.
 * @error Returns -1 if memory allocation fails.
 */
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

/**
 * @brief Creates a new process as a copy of the current process.
 *
 * Copies the parent's address space, file descriptors, and
 * sets up the child to return 0 from fork().
 *
 * @post Child process is in RUNNABLE state.
 * @post Child has copies of parent's open files and cwd.
 *
 * @return PID of child process on success, -1 on failure.
 *
 * @error Returns -1 if process allocation fails.
 * @error Returns -1 if memory copy fails.
 */
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

/**
 * @brief Reparents all children of a process to init.
 *
 * @param p Pointer to the parent process.
 *
 * @pre wait_lock must be held by caller.
 *
 * @post All children of p now have initproc as parent.
 * @post initproc is woken up if needed.
 *
 * @return None.
 */
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

/**
 * @brief Exits the current process with the given exit status.
 *
 * Does not return. The process remains in ZOMBIE state
 * until its parent calls wait().
 *
 * @param status Exit status to be returned to parent.
 *
 * @pre Must not be called from init process.
 *
 * @post All open files are closed.
 * @post Current working directory is released.
 * @post Process state is ZOMBIE.
 * @post Parent is woken up.
 *
 * @note Never returns - jumps to scheduler.
 *
 * @return None.
 *
 * @error Panics if called from init process.
 */
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

/**
 * @brief Waits for a child process to exit and returns its pid.
 *
 * @param addr Address to copy child's exit status to (0 to ignore).
 *
 * @pre Must hold no process locks on entry.
 *
 * @post Child's exit status is copied to addr if addr != 0.
 * @post Child process structure is freed.
 *
 * @return PID of exited child on success, -1 if no children.
 *
 * @error Returns -1 if this process has no children.
 * @error Returns -1 if copyout fails.
 */
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

/**
 * @brief Per-CPU process scheduler.
 *
 * Runs on each CPU and selects a RUNNABLE process to run.
 * Never returns - loops forever selecting and running processes.
 *
 * @post Switches to selected process's context.
 * @post Selected process state changes to RUNNING.
 *
 * @note Uses wait-free algorithm to find runnable processes.
 * @note Enables interrupts briefly to avoid deadlock, then disables.
 * @note Uses WFI (wait for interrupt) when no processes are runnable.
 *
 * @return None.
 *
 * @warning Never returns - infinite loop.
 */
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

/**
 * @brief Switches from process to scheduler.
 *
 * @pre Must hold only p->lock.
 * @pre Process state must have been changed.
 *
 * @post Saves current process context.
 * @post Restores scheduler context.
 *
 * @note Saves and restores intena because it is a property
 *       of the kernel thread, not the CPU.
 *
 * @return None.
 *
 * @error Panics if lock not held, wrong lock count, or interrupts enabled.
 */
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

/**
 * @brief Yields the CPU for one scheduling round.
 *
 * @pre Must be called from a valid process.
 *
 * @post Process state is RUNNABLE.
 * @post Process will run again when scheduled.
 *
 * @return None.
 */
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

/**
 * @brief First function executed by a newly forked child process.
 *
 * Called by scheduler when first switching to a new process.
 * Runs file system initialization if this is the first call.
 *
 * @post File system is initialized on first call.
 * @post Process returns to user space via trampoline.
 *
 * @note Runs fsinit(ROOTDEV) on first invocation only.
 * @note Loads and executes /init program via kexec.
 *
 * @return None.
 */
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == (uint64)-1) {
      panic("exec");
    }
  }

  // return to user space, mimicking usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + ((uint64)userret - (uint64)trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

/**
 * @brief Puts the current process to sleep on a channel.
 *
 * @param chan Sleep channel to block on.
 * @param lk Lock to release while sleeping.
 *
 * @pre Must hold lk lock.
 *
 * @post Process state is SLEEPING.
 * @post Process chan is set to sleep channel.
 * @post lk is released and reacquired around sleep.
 *
 * @note Releases lk before sleeping to allow other processes
 *       to acquire it and potentially wake this process.
 *
 * @return None.
 */
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

/**
 * @brief Wakes up all processes sleeping on a channel.
 *
 * @param chan Sleep channel to wake up.
 *
 * @pre Caller should hold the condition lock.
 *
 * @post All SLEEPING processes on chan are set to RUNNABLE.
 *
 * @note Does not acquire p->lock for non-matching processes
 *       to avoid lock contention.
 *
 * @return None.
 */
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

/**
 * @brief Sends a kill signal to the process with the given PID.
 *
 * @param pid Process ID to kill.
 *
 * @pre No locks held.
 *
 * @post Target process is marked as killed.
 * @post If target was sleeping, it is made RUNNABLE.
 *
 * @return 0 on success, -1 if process not found.
 *
 * @note Process won't exit until it tries to return to user space.
 */
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

/**
 * @brief Sets the killed flag for a process.
 *
 * @param p Pointer to the process.
 *
 * @pre No locks held.
 *
 * @post p->killed is set to 1.
 *
 * @return None.
 */
void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

/**
 * @brief Checks if a process has been killed.
 *
 * @param p Pointer to the process.
 *
 * @return 1 if killed, 0 otherwise.
 */
int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

/**
 * @brief Copies data to either a user or kernel address.
 *
 * @param user_dst If non-zero, copy to user address; else kernel address.
 * @param dst Destination address.
 * @param src Source address.
 * @param len Number of bytes to copy.
 *
 * @post Data is copied to destination.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if user_dst and copyout fails.
 */
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

/**
 * @brief Copies data from either a user or kernel address.
 *
 * @param dst Destination buffer.
 * @param user_src If non-zero, copy from user address; else kernel address.
 * @param src Source address.
 * @param len Number of bytes to copy.
 *
 * @post Data is copied to destination buffer.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Returns -1 if user_src and copyin fails.
 */
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

/**
 * @brief Dumps process information to console for debugging.
 *
 * Called when user types ^P on console.
 * Displays PID, state, and name for each process.
 *
 * @note No lock used to avoid wedging a stuck machine.
 *
 * @return None.
 */
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
