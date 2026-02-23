/**
 * @file bio.c
 * @brief Buffer cache implementation.
 *
 * The buffer cache is a linked list of buf structures holding
 * cached copies of disk block contents. Caching disk blocks
 * in memory reduces the number of disk reads and also provides
 * a synchronization point for disk blocks used by multiple processes.
 *
 * Interface:
 * * To get a buffer for a particular disk block, call bread().
 * * After changing buffer data, call bwrite() to write it to disk.
 * * When done with the buffer, call brelse().
 * * Do not use the buffer after calling brelse().
 * * Only one process at a time can use a buffer,
 *     so do not keep them longer than necessary.
 */

#include "core/types.h"
#include "core/param.h"
#include "core/spinlock.h"
#include "core/sleeplock.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "fs/fs.h"
#include "fs/buf.h"

/**
 * @brief Buffer cache structure.
 */
struct {
  /** @brief Lock protecting cache structure. */
  struct spinlock lock;
  /** @brief Array of buffer structures. */
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  /** @brief LRU list head (sentinel). */
  struct buf head;
} bcache;

/**
 * @brief Initializes the buffer cache.
 *
 * Sets up the LRU linked list and initializes locks.
 *
 * @post All buffers linked in LRU list.
 * @post Each buffer has a sleep lock initialized.
 *
 * @return None.
 */
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

/**
 * @brief Gets a buffer for a specific disk block.
 *
 * Looks through buffer cache for block on device dev.
 * If not found, allocates a buffer using LRU replacement.
 *
 * @param dev     Device number.
 * @param blockno Block number on device.
 *
 * @post Returned buffer is locked.
 * @post Buffer reference count incremented.
 *
 * @return Pointer to locked buffer.
 *
 * @error Panics if no buffers available.
 */
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

/**
 * @brief Returns a locked buffer with disk block contents.
 *
 * @param dev     Device number.
 * @param blockno Block number on device.
 *
 * @post Returned buffer is locked.
 * @post If not valid, reads data from disk.
 *
 * @return Pointer to locked buffer.
 */
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

/**
 * @brief Writes buffer contents to disk.
 *
 * @param b Buffer to write.
 *
 * @pre Buffer must be locked.
 *
 * @post Data written to disk synchronously.
 *
 * @return None.
 *
 * @error Panics if buffer is not locked.
 */
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

/**
 * @brief Releases a locked buffer.
 *
 * @param b Buffer to release.
 *
 * @pre Buffer must be locked.
 *
 * @post Buffer lock released.
 * @post If refcnt is 0, buffer moved to front of LRU list.
 *
 * @return None.
 *
 * @error Panics if buffer is not locked.
 */
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

/**
 * @brief Pins a buffer in cache (prevents eviction).
 *
 * @param b Buffer to pin.
 *
 * @post Buffer refcnt incremented.
 *
 * @return None.
 */
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

/**
 * @brief Unpins a buffer in cache (allows eviction).
 *
 * @param b Buffer to unpin.
 *
 * @post Buffer refcnt decremented.
 *
 * @return None.
 */
void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}
