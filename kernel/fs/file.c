/**
 * @file file.c
 * @brief File descriptor support functions implementation.
 *
 * Provides file allocation, duplication, closing, and I/O operations
 * for the file descriptor based system calls.
 */

#include "core/types.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/param.h"
#include "fs/fs.h"
#include "core/spinlock.h"
#include "core/sleeplock.h"
#include "fs/file.h"
#include "fs/stat.h"
#include "core/proc.h"

/**
 * @brief Global device switch table.
 */
struct devsw devsw[NDEV];

/**
 * @brief File table containing all open files.
 */
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

/**
 * @brief Initializes the file table.
 *
 * @post File table lock is initialized.
 *
 * @return None.
 */
void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

/**
 * @brief Allocates a free file structure.
 *
 * Searches the file table for an unused entry with ref == 0.
 *
 * @post Returned file has ref set to 1.
 *
 * @return Pointer to allocated file on success, NULL on failure.
 */
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

/**
 * @brief Increments the reference count for a file.
 *
 * @param f File to duplicate.
 *
 * @post f->ref is incremented by 1.
 *
 * @return Pointer to the file.
 *
 * @error Panics if ref count is less than 1.
 */
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

/**
 * @brief Closes a file, decrementing reference count.
 *
 * @param f File to close.
 *
 * @post If ref reaches 0, underlying resource is released.
 * @post For pipes: pipe is closed.
 * @post For inodes/devices: inode reference is released via iput().
 *
 * @return None.
 *
 * @error Panics if ref count is less than 1.
 */
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

/**
 * @brief Gets metadata about an open file.
 *
 * @param f   Open file to query.
 * @param addr User virtual address to store struct stat.
 *
 * @pre File must be FD_INODE or FD_DEVICE type.
 *
 * @post stat structure copied to user memory.
 *
 * @return 0 on success, -1 on failure.
 */
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

/**
 * @brief Reads from an open file.
 *
 * @param f    Open file to read from.
 * @param addr User buffer address.
 * @param n    Number of bytes to read.
 *
 * @pre File must be readable.
 *
 * @post File offset advanced by number of bytes read.
 *
 * @return Number of bytes read on success, -1 on error.
 *
 * @error Returns -1 if file is not readable.
 * @error Returns -1 for invalid device.
 */
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

/**
 * @brief Writes to an open file.
 *
 * @param f    Open file to write to.
 * @param addr User buffer address.
 * @param n    Number of bytes to write.
 *
 * @pre File must be writable.
 *
 * @post File offset advanced by number of bytes written.
 *
 * @return Number of bytes written on success, -1 on error.
 *
 * @error Returns -1 if file is not writable.
 * @error Returns -1 for invalid device.
 *
 * @note Splits large writes into multiple transactions to avoid
 *       exceeding the logging subsystem's maximum transaction size.
 */
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}
