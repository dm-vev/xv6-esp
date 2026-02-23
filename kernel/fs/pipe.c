/**
 * @file pipe.c
 * @brief Pipe implementation for inter-process communication.
 */

#include "core/types.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/param.h"
#include "core/spinlock.h"
#include "core/proc.h"
#include "fs/fs.h"
#include "core/sleeplock.h"
#include "fs/file.h"

/** @brief Size of pipe buffer in bytes. */
#define PIPESIZE 512

/**
 * @brief Pipe structure.
 *
 * Circular buffer for passing data between processes.
 */
struct pipe {
  /** @brief Lock protecting pipe data. */
  struct spinlock lock;
  /** @brief Data buffer. */
  char data[PIPESIZE];
  /** @brief Number of bytes read. */
  uint nread;
  /** @brief Number of bytes written. */
  uint nwrite;
  /** @brief Whether read end is open. */
  int readopen;
  /** @brief Whether write end is open. */
  int writeopen;
};

/**
 * @brief Allocates a new pipe.
 *
 * Creates a pipe with two file structures for reading and writing.
 *
 * @param f0 Pointer to store read end file pointer.
 * @param f1 Pointer to store write end file pointer.
 *
 * @post Two file structures allocated and configured.
 *
 * @return 0 on success, -1 on failure.
 *
 * @error Frees all allocated resources on failure.
 */
int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

/**
 * @brief Closes one end of a pipe.
 *
 * @param pi       Pipe to close.
 * @param writable If non-zero, close write end; else close read end.
 *
 * @post If both ends closed, pipe buffer is freed.
 * @post Waiters are woken up.
 *
 * @return None.
 */
void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

/**
 * @brief Writes data to a pipe.
 *
 * @param pi   Pipe to write to.
 * @param addr User buffer address.
 * @param n    Number of bytes to write.
 *
 * @post Data copied to pipe buffer.
 * @post Writers are woken up when buffer has space.
 *
 * @return Number of bytes written on success.
 * @return -1 if read end closed or process killed.
 *
 * @error Returns -1 if reader closed while waiting.
 */
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1){
        if(i == 0)
          i = -1;
        break;
      }
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

/**
 * @brief Reads data from a pipe.
 *
 * @param pi   Pipe to read from.
 * @param addr User buffer address.
 * @param n    Maximum bytes to read.
 *
 * @post Data copied to user buffer.
 * @post Readers woken up when data is available.
 *
 * @return Number of bytes read.
 * @return -1 if write end closed.
 */
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1) {
      if(i == 0)
        i = -1;
      break;
    }
    pi->nread++;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
