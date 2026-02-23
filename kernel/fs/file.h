/**
 * @file file.h
 * @brief File and inode data structures.
 */
#ifndef FILE_H
#define FILE_H

/**
 * @brief File structure representing an open file.
 *
 * Can represent a pipe, inode, or device.
 */
struct file {
  /** @brief Type of file: FD_NONE, FD_PIPE, FD_INODE, or FD_DEVICE. */
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
  /** @brief Reference count for this file structure. */
  int ref;
  /** @brief Whether the file is readable. */
  char readable;
  /** @brief Whether the file is writable. */
  char writable;
  /** @brief Pipe pointer for FD_PIPE type. */
  struct pipe *pipe;
  /** @brief Inode pointer for FD_INODE and FD_DEVICE types. */
  struct inode *ip;
  /** @brief Current offset for FD_INODE type. */
  uint off;
  /** @brief Device major number for FD_DEVICE type. */
  short major;
};

/**
 * @brief Extracts major device number from device id.
 */
#define major(dev)  ((dev) >> 16 & 0xFFFF)

/**
 * @brief Extracts minor device number from device id.
 */
#define minor(dev)  ((dev) & 0xFFFF)

/**
 * @brief Creates a device id from major and minor numbers.
 */
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

/**
 * @brief In-memory copy of an inode.
 *
 * Cached copy of disk inode with additional metadata.
 */
struct inode {
  /** @brief Device number. */
  uint dev;
  /** @brief Inode number. */
  uint inum;
  /** @brief Reference count. */
  int ref;
  /** @brief Sleep lock protecting fields below. */
  struct sleeplock lock;
  /** @brief Whether inode has been read from disk. */
  int valid;

  /** @brief File type from disk inode. */
  short type;
  /** @brief Major device number. */
  short major;
  /** @brief Minor device number. */
  short minor;
  /** @brief Number of hard links. */
  short nlink;
  /** @brief File size in bytes. */
  uint size;
  /** @brief Block addresses (direct and indirect). */
  uint addrs[NDIRECT+1];
};

/**
 * @brief Device switch table entry.
 *
 * Maps device operations to handler functions.
 */
struct devsw {
  /** @brief Device read handler. */
  int (*read)(int, uint64, int);
  /** @brief Device write handler. */
  int (*write)(int, uint64, int);
};

/** @brief Global device switch table. */
extern struct devsw devsw[];

/** @brief Console device number. */
#define CONSOLE 1

#endif // FILE_H
