/**
 * @file vfs_fd.h
 * @brief File descriptor management
 *
 * Manages the VFS file descriptor table, which maps integer file descriptors
 * to underlying file, device, or pipe objects. Each FD entry stores:
 * - File type (file, device, pipe)
 * - Open flags (read-only, write-only, etc.)
 * - For devices: role (PTY master/slave) and ID
 * - For files: inode number and offset
 * - Owner task handle for access control
 */
#ifndef XV6_VFS_FD_H
#define XV6_VFS_FD_H

#include "core/types.h"
#include "core/param.h"

/**
 * @brief FD is free/available
 */
#define VFD_FREE 0

/**
 * @brief FD refers to a regular file (on flash filesystem)
 */
#define VFD_FILE 1

/**
 * @brief FD refers to a device file (/dev/xxx)
 */
#define VFD_DEV 2

/**
 * @brief FD refers to a pipe
 */
#define VFD_PIPE 3

/**
 * @brief No special device role
 */
#define DEV_ROLE_NONE 0

/**
 * @brief Device is a PTY master
 */
#define DEV_ROLE_PTY_MASTER 1

/**
 * @brief Device is a PTY slave
 */
#define DEV_ROLE_PTY_SLAVE 2

/**
 * @brief File descriptor table entry
 *
 * Represents an open file, device, or pipe in the VFS.
 */
typedef struct {
  int used;           /**< Whether this FD slot is in use */
  int kind;           /**< Type: VFD_FILE, VFD_DEV, VFD_PIPE */
  int flags;          /**< Open flags: XV6_O_RDONLY, XV6_O_WRONLY, etc. */
  int dev_role;       /**< Device role for VFD_DEV: DEV_ROLE_* */
  int dev_id;         /**< Device ID (PTY id, pipe id, etc.) */
  int group_id;       /**< FD group for close-on-exec */
  void *owner;        /**< Owner FreeRTOS task handle */
  uint32 inum;        /**< Inode number for VFD_FILE */
  uint32 off;         /**< Current file offset */
  char path[MAXPATH]; /**< Canonical path for debugging */
} xv6_vfd_t;

/** Global FD table (defined in vfs_fd.c) */
extern xv6_vfd_t g_fds[];

/**
 * @brief Initialize the file descriptor table
 *
 * Sets up the FD table and creates standard file descriptors 0, 1, 2
 * for stdin, stdout, stderr pointing to /dev/console.
 *
 * @return 0 on success
 */
int vfs_fd_init(void);

/**
 * @brief Allocate a new file descriptor
 *
 * Finds the first free slot in the FD table and marks it as used.
 * Does not initialize the FD entry - caller must do that.
 *
 * @return File descriptor number (0-127), or -1 if table is full
 */
int vfs_fd_alloc(void);

/**
 * @brief Get file descriptor entry
 *
 * Returns a pointer to the FD table entry for the given descriptor.
 * Does not check if the FD is valid - caller must verify.
 *
 * @param fd File descriptor number
 * @return Pointer to FD entry, or NULL if fd is out of range
 */
xv6_vfd_t *vfs_fd_get(int fd);

/**
 * @brief Free a file descriptor
 *
 * Marks the FD slot as free and clears its contents.
 * Does not perform any cleanup of the underlying file/device.
 *
 * @param fd File descriptor to free
 */
void vfs_fd_free(int fd);

/**
 * @brief Duplicate a file descriptor
 *
 * Creates a new FD that refers to the same file/device as the original.
 * The new FD is allocated from the lowest available slot.
 *
 * @param oldfd Original file descriptor to duplicate
 * @return New file descriptor, or -1 on error (invalid fd or no free slots)
 */
int vfs_fd_dup(int oldfd);

#endif
