/**
 * @file vfs_fd.h
 * @brief File descriptor management
 *
 * Manages the VFS file descriptor table.
 */
#ifndef XV6_VFS_FD_H
#define XV6_VFS_FD_H

#include "core/types.h"
#include "core/param.h"

/**
 * @brief VFD kinds
 */
#define VFD_FREE 0
#define VFD_FILE 1
#define VFD_DEV 2
#define VFD_PIPE 3

/**
 * @brief Device roles
 */
#define DEV_ROLE_NONE 0
#define DEV_ROLE_PTY_MASTER 1
#define DEV_ROLE_PTY_SLAVE 2

/**
 * @brief File descriptor entry
 */
typedef struct {
  int used;         /**< In use flag */
  int kind;         /**< VFD_* type */
  int flags;        /**< Open flags */
  int dev_role;     /**< Device role */
  int dev_id;       /**< Device ID */
  int group_id;     /**< FD group */
  void *owner;      /**< Owner task */
  uint32 inum;      /**< Inode number */
  uint32 off;       /**< File offset */
  char path[MAXPATH]; /**< Path */
} xv6_vfd_t;

/**
 * @brief Initialize FD subsystem
 * @return 0 on success
 */
int vfs_fd_init(void);

/**
 * @brief Allocate a file descriptor
 * @return FD number or -1
 */
int vfs_fd_alloc(void);

/**
 * @brief Get FD entry
 * @param fd File descriptor
 * @return Pointer to FD entry or NULL
 */
xv6_vfd_t *vfs_fd_get(int fd);

/**
 * @brief Free a file descriptor
 * @param fd File descriptor
 */
void vfs_fd_free(int fd);

/**
 * @brief Duplicate FD
 * @param oldfd Original FD
 * @return New FD or -1
 */
int vfs_fd_dup(int oldfd);

#endif
