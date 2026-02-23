/**
 * @file log.c
 * @brief Simple logging implementation for file system.
 *
 * A log transaction contains the updates of multiple FS system
 * calls. The logging system only commits when there are
 * no FS system calls active. Thus there is never
 * any reasoning required about whether a commit might
 * write an uncommitted system call's updates to disk.
 *
 * A system call should call begin_op()/end_op() to mark
 * its start and end. Usually begin_op() just increments
 * the count of in-progress FS system calls and returns.
 * But if it thinks the log is close to running out, it
 * sleeps until the last outstanding end_op() commits.
 *
 * The log is a physical re-do log containing disk blocks.
 * The on-disk log format:
 *   header block, containing block #s for block A, B, C, ...
 *   block A
 *   block B
 *   block C
 *   ...
 * Log appends are synchronous.
 */

#include "core/types.h"
#include "arch/riscv.h"
#include "core/defs.h"
#include "core/param.h"
#include "core/spinlock.h"
#include "core/sleeplock.h"
#include "fs/fs.h"
#include "fs/buf.h"

/**
 * @brief Log header structure.
 *
 * Used for both on-disk header and in-memory tracking.
 */
struct logheader {
  /** @brief Number of logged blocks. */
  int n;
  /** @brief Array of block numbers being logged. */
  int block[LOGBLOCKS];
};

/**
 * @brief In-memory log state structure.
 */
struct log {
  /** @brief Lock protecting log state. */
  struct spinlock lock;
  /** @brief Starting block number of log on disk. */
  int start;
  /** @brief Number of FS sys calls currently executing. */
  int outstanding;
  /** @brief Whether commit is in progress. */
  int committing;
  /** @brief Device number. */
  int dev;
  /** @brief In-memory log header. */
  struct logheader lh;
};

/** @brief Global log structure. */
struct log log;

static void recover_from_log(void);
static void commit(void);

/**
 * @brief Initializes the logging system.
 *
 * @param dev Device number.
 * @param sb  Superblock containing log information.
 *
 * @post Log is initialized and recovery is performed.
 *
 * @return None.
 *
 * @error Panics if log header is too large for a block.
 */
void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.dev = dev;
  recover_from_log();
}

/**
 * @brief Copies committed blocks from log to their home location.
 *
 * @param recovering If non-zero, recovery mode is active.
 *
 * @post Blocks copied from log to final location.
 *
 * @return None.
 */
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    if(recovering) {
      printf("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
    }
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

/**
 * @brief Reads the log header from disk.
 *
 * @post In-memory log header populated from disk.
 *
 * @return None.
 */
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

/**
 * @brief Writes the in-memory log header to disk.
 *
 * This is point at which the current the true transaction commits.
 *
 * @post Log header written to disk.
 *
 * @return None.
 */
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

/**
 * @brief Recovers from a previous crash by replaying log.
 *
 * @post Committed transactions are replayed.
 * @post Log is cleared.
 *
 * @return None.
 */
static void
recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

/**
 * @brief Marks the start of a file system system call.
 *
 * Increments the count of in-progress FS system calls.
 * May sleep if log is close to full.
 *
 * @post outstanding incremented.
 *
 * @return None.
 */
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGBLOCKS){
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

/**
 * @brief Marks the end of a file system system call.
 *
 * Commits the transaction if this was the last outstanding operation.
 *
 * @post outstanding decremented.
 * @post May trigger commit if last operation.
 *
 * @return None.
 *
 * @error Panics if already committing.
 */
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

/**
 * @brief Copies modified blocks from cache to log.
 *
 * @post Blocks copied to log area on disk.
 *
 * @return None.
 */
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

/**
 * @brief Commits the current transaction.
 *
 * Writes modified blocks to log, commits via header write,
 * installs to final location, then clears log.
 *
 * @post Log is cleared after commit.
 *
 * @return None.
 */
static void
commit(void)
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

/**
 * @brief Logs a buffer modification for commit.
 *
 * Caller has modified b->data and is done with the buffer.
 * Record the block number and pin in the cache by increasing refcnt.
 * commit()/write_log() will do the disk write.
 *
 * log_write() replaces bwrite(); a typical use is:
 *   bp = bread(...)
 *   modify bp->data[]
 *   log_write(bp)
 *   brelse(bp)
 *
 * @param b Buffer to log.
 *
 * @pre Must be called within a transaction (between begin_op/end_op).
 *
 * @post Block added to log if not already present.
 * @post Buffer pinned in cache.
 *
 * @return None.
 *
 * @error Panics if transaction too large or outside transaction.
 */
void
log_write(struct buf *b)
{
  int i;

  acquire(&log.lock);
  if (log.lh.n >= LOGBLOCKS)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorption
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}
