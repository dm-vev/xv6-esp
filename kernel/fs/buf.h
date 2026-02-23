/**
 * @file buf.h
 * @brief Buffer cache data structure.
 */
#ifndef BUF_H
#define BUF_H

/**
 * @brief Buffer structure for disk block caching.
 *
 * Used to cache disk blocks in memory for faster access.
 */
struct buf {
  /** @brief Whether data has been read from disk. */
  int valid;
  /** @brief Whether disk is currently using this buffer. */
  int disk;
  /** @brief Device number. */
  uint dev;
  /** @brief Block number on disk. */
  uint blockno;
  /** @brief Sleep lock protecting buffer. */
  struct sleeplock lock;
  /** @brief Reference count. */
  uint refcnt;
  /** @brief Previous buffer in LRU list. */
  struct buf *prev;
  /** @brief Next buffer in LRU list. */
  struct buf *next;
  /** @brief Disk block data (BSIZE bytes). */
  uchar data[BSIZE];
};

#endif // BUF_H
